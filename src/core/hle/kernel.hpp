#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "common/types.hpp"
#include "arm64/interpreter.hpp"
#include "memory.hpp"
#include "kernel_objects.hpp"
#include "service.hpp"
#include "display.hpp"
#include "input.hpp"
#include "video_core/gpu.hpp"
#include <array>

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
    // TLS de los demas hilos: paginas hacia abajo desde TLS_PAGE (8 hilos por pagina)
    constexpr u64 TLS_SLOT_SIZE     = 0x200;
    constexpr u64 TLS_MAX_PAGES     = 15;
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
    constexpr u32 InvalidPriority  = 0xE001;
    constexpr u32 InvalidCoreId    = 0xE201;
    constexpr u32 OutOfResource    = 0xD201;
    constexpr u32 Cancelled        = 0xEC01;
    constexpr u32 OutOfRange       = 0xEE01;
    constexpr u32 InvalidCurrentMemory = 0xD401;
}

// Handles "falsos" que entregamos al programa.
constexpr u32 CURRENT_THREAD_PSEUDO_HANDLE  = 0xFFFF8000;
constexpr u32 CURRENT_PROCESS_PSEUDO_HANDLE = 0xFFFF8001;
constexpr u32 MAIN_THREAD_HANDLE            = 0x00010001;

// Nucleos que ve el programa (CoreMask 0x3F). La Switch 2 tiene 8; 2 son del sistema.
constexpr s32 NUM_CORES = 6;
// Instrucciones que corre un hilo antes de pasar al siguiente nucleo
constexpr u64 SCHEDULER_SLICE = 10'000;
// Bit del mutex de libnx/Horizon: "hay hilos esperando este mutex"
constexpr u32 MUTEX_HAS_WAITERS = 0x40000000;

class Kernel {
public:
    // Se registra como manejador de SVC del interprete.
    Kernel(Core::Memory& memory, Core::Interpreter& cpu);
    ~Kernel() { for (auto& t : m_threads) t->wait_objects.clear(); }

    // Vuelve al estado inicial (sin heap, sin salida de texto).
    void Reset();

    // Prepara un proceso homebrew segun el "Homebrew ABI" (switchbrew.org/wiki/Homebrew_ABI):
    // crea pila, TLS y la lista de configuracion del loader, y pone los registros
    // de entrada (X0 = config, X1 = -1, SP = pila, X30 = stub que llama a svcExitProcess).
    void SetupHomebrewProcess(u64 entry, u64 image_base, u64 image_size, const std::string& argv);

    // Llamada desde el interprete al ejecutar "svc #imm".
    void HandleSvc(u32 imm, Core::CPUState& state);

    // --- Hilos y planificador (kernel_threads.cpp) ---
    // Ejecuta hasta 'budget' instrucciones repartidas entre los hilos listos
    // (6 nucleos emulados que se turnan). Devuelve las instrucciones ejecutadas.
    u64 Run(u64 budget);
    const std::vector<std::shared_ptr<KThread>>& Threads() const { return m_threads; }
    const KThread* CurrentThread() const { return m_current.get(); }

    // Contadores para la interfaz y los tests
    struct SchedulerStats {
        u64 context_switches = 0;   // cambios de hilo en la CPU
        u64 mutex_waits = 0;        // hilos que tuvieron que esperar un mutex
        u64 condvar_waits = 0;      // esperas en variables de condicion
        u64 sync_waits = 0;         // esperas en svcWaitSynchronization que bloquearon
        u64 idle_ticks = 0;         // ticks saltados porque todos dormian
        u64 gpu_waits = 0;          // veces que todos esperaban y la GPU aun trabajaba
    };
    const SchedulerStats& Stats() const { return m_stats; }
    s32 CurrentCore() const { return m_currentCore; }

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

    // GPU emulada (canales, memoria de la GPU, syncpoints): video_core/gpu.hpp
    GPU::Gpu& GetGpu() { return m_gpu; }
    // Eventos de nvhost-ctrl (EVENT_WAIT_ASYNC): uno por "slot", los crea QueryEvent
    std::shared_ptr<KEvent>& NvEventSlot(u32 slot) { return m_nvEvents[slot & 0xFF]; }

    // Carpeta del PC que hace de tarjeta SD ("sdmc:/"). Por defecto "sdmc" en el directorio actual.
    void SetSdmcRoot(const std::filesystem::path& root) { m_sdmcRoot = root; }
    const std::filesystem::path& GetSdmcRoot() const { return m_sdmcRoot; }
    // El propio .nro, visible en la SD como "/switch/<nombre>.nro" (solo lectura). Asi
    // romfsInit() de libnx lo encuentra con argv[0], como cuando lo lanza el hbmenu.
    void SetSelfNro(std::string guest_path, std::vector<u8> data) {
        m_selfNroPath = std::move(guest_path);
        m_selfNro = std::make_shared<const std::vector<u8>>(std::move(data));
    }
    const std::string& GetSelfNroPath() const { return m_selfNroPath; }
    std::shared_ptr<const std::vector<u8>> GetSelfNro() const { return m_selfNro; }

    // Mandos: memoria compartida de hid (una por proceso) y estado nuevo de los botones
    u32  GetHidSharedMemoryHandle();
    void SetPadInput(const PadInput& pad);
    const InputState& GetInput() const { return m_input; }

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

    // --- Hilos (kernel_threads.cpp) ---
    void SvcCreateThread(Core::CPUState& s);
    void SvcStartThread(Core::CPUState& s);
    void SvcExitThread(Core::CPUState& s);
    void SvcGetThreadPriority(Core::CPUState& s);
    void SvcSetThreadPriority(Core::CPUState& s);
    void SvcGetThreadCoreMask(Core::CPUState& s);
    void SvcSetThreadCoreMask(Core::CPUState& s);
    void SvcGetThreadId(Core::CPUState& s);
    void SvcSignalEvent(Core::CPUState& s);
    void SvcCancelSynchronization(Core::CPUState& s);
    void SvcArbitrateLock(Core::CPUState& s);
    void SvcArbitrateUnlock(Core::CPUState& s);
    void SvcWaitProcessWideKeyAtomic(Core::CPUState& s);
    void SvcSignalProcessWideKey(Core::CPUState& s);

    std::shared_ptr<KThread> CreateMainThread(u64 tls);
    std::shared_ptr<KThread> ThreadFromHandle(u32 handle);   // acepta 0xFFFF8000 (hilo actual)
    Core::CPUState& Ctx(KThread& t);       // registros del hilo (en la CPU si es el actual)
    void Block(KThread& t, KThread::Wait kind, s64 timeout_ns);
public:
    // Duerme el hilo actual hasta 'deadline' (ticks) sin tocar su X0: para que un servicio
    // (vi) haga esperar al programa despues de contestarle, como la sincronizacion vertical.
    void SleepCurrentUntil(u64 deadline);
    u64  Ticks() const { return m_cpu.GetTicks(); }   // reloj del sistema (CNTPCT)
    // Siguiente sincronizacion vertical (60 Hz) despues de 'now', en ticks
    static u64 NextVsync(u64 now) {
        constexpr u64 PERIOD = Core::Interpreter::TICK_FREQUENCY / 60;
        return (now / PERIOD + 1) * PERIOD;
    }
private:
    void Wake(KThread& t, u32 result);
    void ReleaseMutex(u64 addr);           // da el mutex al siguiente que lo espera (o lo deja libre)
    void AcquireMutexAfterWait(KThread& t, u32 result);
    void UpdateWaits();                    // plazos vencidos y objetos activados
    KThread* PickNext();
    void SwitchTo(const std::shared_ptr<KThread>& t);
    u64  AllocateTls();
    void FreeTls(u64 tls);
    u64  Now() const { return m_cpu.GetTicks(); }
    static u64 NsToTicks(s64 ns);

    // Comandos "Control" de CMIF (dominios, clonar sesiones, tamano de buffer)
    void HandleControlCommand(IpcContext& ctx, const std::shared_ptr<SessionState>& state);
    void RegisterDefaultServices();

    Core::Memory&      m_memory;
    Core::Interpreter& m_cpu;

    HandleTable     m_handles;
    ServiceRegistry m_services;
    Display         m_display;
    GPU::Gpu        m_gpu{m_memory};
    std::array<std::shared_ptr<KEvent>, 256> m_nvEvents{};
    InputState      m_input;
    std::shared_ptr<KSharedMemory> m_hidMemory;
    std::filesystem::path m_sdmcRoot = "sdmc";
    std::string m_selfNroPath;
    std::shared_ptr<const std::vector<u8>> m_selfNro;

    std::vector<std::shared_ptr<KThread>> m_threads;
    std::shared_ptr<KThread> m_current;    // el hilo cuyos registros estan en la CPU
    s32  m_currentCore = 0;
    u64  m_nextThreadId = 1;
    u64  m_waitCounter = 0;
    u64  m_runCounter = 0;
    std::vector<u64> m_freeTls;
    u64  m_tlsPagesUsed = 0;
    u64  m_tlsNextSlot = 0;
    SchedulerStats m_stats;

    u64  m_heapSize = 0;
    u64  m_imageSize = 0;
    bool m_exited = false;
    std::string m_debugOutput;
};

} // namespace NeXo2::HLE
