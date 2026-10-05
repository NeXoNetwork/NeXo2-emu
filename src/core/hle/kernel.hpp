#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include "common/types.hpp"
#include "arm64/interpreter.hpp"
#include "memory.hpp"
#include "kernel_objects.hpp"
#include "service.hpp"
#include "display.hpp"

// Kernel HLE (High-Level Emulation) de Horizon.
//
// En vez de ejecutar el kernel real de la consola, cuando el programa hace
// "svc #n" el interprete nos llama y nosotros hacemos lo que haria el kernel
// (reservar heap, imprimir texto, terminar el proceso...) directamente en C++.
// Referencia de cada SVC: docs/02-horizon-os/svc.md y switchbrew.org/wiki/SVC
namespace NeXo2::HLE {

// Mapa de memoria del proceso. Valores fijos (sin ASLR) y dentro de los 12 GB
// que soporta Core::Memory. Los programas los consultan con svcGetInfo.
namespace Layout {
    constexpr u64 CODE_BASE        = 0x0000'0000'0800'0000; // donde se carga el NRO
    constexpr u64 ASLR_REGION_BASE = 0x0000'0000'0800'0000;
    constexpr u64 ASLR_REGION_SIZE = 0x0000'0002'F800'0000; // hasta 0x3'0000'0000 (12 GB)
    constexpr u64 HEAP_REGION_BASE = 0x0000'0000'8000'0000;
    constexpr u64 HEAP_REGION_SIZE = 0x0000'0000'4000'0000; // 1 GB
    constexpr u64 ALIAS_REGION_BASE = 0x0000'0000'C000'0000;
    constexpr u64 ALIAS_REGION_SIZE = 0x0000'0000'4000'0000;
    constexpr u64 STACK_REGION_BASE = 0x0000'0001'0000'0000;
    constexpr u64 STACK_REGION_SIZE = 0x0000'0000'1000'0000;
    constexpr u64 MAIN_STACK_SIZE   = 0x0000'0000'0010'0000; // 1 MB
    constexpr u64 LOADER_PAGE       = 0x0000'0001'FFFE'0000; // stub de salida + config del loader
    constexpr u64 TLS_PAGE          = 0x0000'0001'FFFF'0000; // TLS del hilo principal
    constexpr u64 TOTAL_MEMORY      = HEAP_REGION_SIZE;      // lo que "tiene" el proceso
}

// Codigos de resultado del kernel (modulo 1). Formato: (descripcion << 9) | modulo.
namespace Result {
    constexpr u32 Success          = 0;
    constexpr u32 InvalidSize      = 0xCA01;
    constexpr u32 InvalidAddress   = 0xCC01;
    constexpr u32 OutOfMemory      = 0xD001;
    constexpr u32 InvalidHandle    = 0xE401;
    constexpr u32 InvalidEnumValue = 0xF001;
    constexpr u32 NotFound         = 0xF201;
    constexpr u32 TimedOut         = 0xEA01;
    constexpr u32 InvalidState     = 0xFA01;
    constexpr u32 InvalidCombination = 0xE801;
}

// Handles "falsos" que entregamos al programa.
constexpr u32 CURRENT_THREAD_PSEUDO_HANDLE  = 0xFFFF8000;
constexpr u32 CURRENT_PROCESS_PSEUDO_HANDLE = 0xFFFF8001;
constexpr u32 MAIN_THREAD_HANDLE            = 0x00010001;

class Kernel {
public:
    // Se registra como manejador de SVC del interprete.
    Kernel(Core::Memory& memory, Core::Interpreter& cpu);

    // Vuelve al estado inicial (sin heap, sin salida de texto).
    void Reset();

    // Prepara un proceso homebrew segun el "Homebrew ABI" (switchbrew.org/wiki/Homebrew_ABI):
    // crea pila, TLS y la lista de configuracion del loader, y pone los registros
    // de entrada (X0 = config, X1 = -1, SP = pila, X30 = stub que llama a svcExitProcess).
    void SetupHomebrewProcess(u64 entry, u64 image_base, u64 image_size, const std::string& argv);

    // Llamada desde el interprete al ejecutar "svc #imm".
    void HandleSvc(u32 imm, Core::CPUState& state);

    // Texto que el programa ha enviado con svcOutputDebugString.
    const std::string& GetDebugOutput() const { return m_debugOutput; }
    bool HasExited() const { return m_exited; }
    u64  GetHeapSize() const { return m_heapSize; }

    // Nombre de una SVC para mensajes ("SetHeapSize"...). "?" si no la conocemos.
    static const char* SvcName(u32 imm);

    // --- IPC y servicios (los usan sm: y el codigo de ipc.cpp) ---
    ServiceRegistry& Services() { return m_services; }
    HandleTable&     Handles()  { return m_handles; }

    // Crea una sesion nueva con 'service' y devuelve su handle.
    u32 CreateSessionHandle(std::shared_ptr<ServiceObject> service);

    // Un servicio ha recibido un comando que no existe: para la CPU con el detalle.
    void ReportUnimplemented(const std::string& service_name, u32 command_id);
    void HaltWithMessage(const std::string& message);

    // Atiende un mensaje IPC que esta en 'message' para la sesion 'handle'.
    // Devuelve el resultado de la SVC (no el del comando, que va dentro del mensaje).
    u32 ProcessIpcRequest(u32 handle, u64 message);

    // Pantalla emulada (memoria nvmap + ultima imagen presentada)
    Display&       GetDisplay()       { return m_display; }
    const Display& GetDisplay() const { return m_display; }

    // Carpeta del PC que hace de tarjeta SD ("sdmc:/"). Por defecto "sdmc" en el directorio actual.
    void SetSdmcRoot(const std::filesystem::path& root) { m_sdmcRoot = root; }
    const std::filesystem::path& GetSdmcRoot() const { return m_sdmcRoot; }

    // Objetos que los servicios entregan como handles "copy"
    u32 CreateEvent(const std::string& name, bool signaled = false);
    // Devuelve el handle y deja en 'out' el objeto para rellenar su contenido.
    u32 CreateSharedMemory(const std::string& name, size_t size, std::shared_ptr<KSharedMemory>* out = nullptr);

private:
    // Cada SVC lee sus argumentos de X0..X7 y deja el resultado en W0 (+ salidas en X1...).
    void SvcSetHeapSize(Core::CPUState& s);
    void SvcSetMemoryPermission(Core::CPUState& s);
    void SvcQueryMemory(Core::CPUState& s);
    void SvcExitProcess(Core::CPUState& s);
    void SvcSleepThread(Core::CPUState& s);
    void SvcCloseHandle(Core::CPUState& s);
    void SvcGetSystemTick(Core::CPUState& s);
    void SvcBreak(Core::CPUState& s);
    void SvcOutputDebugString(Core::CPUState& s);
    void SvcGetInfo(Core::CPUState& s);
    void SvcConnectToNamedPort(Core::CPUState& s);
    void SvcSendSyncRequest(Core::CPUState& s);
    void SvcSendSyncRequestWithUserBuffer(Core::CPUState& s);
    void SvcClearEvent(Core::CPUState& s);
    void SvcWaitSynchronization(Core::CPUState& s);
    void SvcMapSharedMemory(Core::CPUState& s);
    void SvcUnmapSharedMemory(Core::CPUState& s);

    // Comandos "Control" de CMIF (dominios, clonar sesiones, tamano de buffer)
    void HandleControlCommand(IpcContext& ctx, const std::shared_ptr<SessionState>& state);
    void RegisterDefaultServices();

    Core::Memory&      m_memory;
    Core::Interpreter& m_cpu;

    HandleTable     m_handles;
    ServiceRegistry m_services;
    Display         m_display;
    std::filesystem::path m_sdmcRoot = "sdmc";

    u64  m_heapSize = 0;
    u64  m_imageSize = 0;
    bool m_exited = false;
    std::string m_debugOutput;
};

} // namespace NeXo2::HLE
