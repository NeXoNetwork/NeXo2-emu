#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
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

class NetworkState;   // services/bsd.hpp

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
    constexpr u64 NEXT_LOAD_PATH    = LOADER_PAGE + 0x600;   // envSetNextLoad: ruta (0x200 bytes)
    constexpr u64 NEXT_LOAD_PATH_SIZE = 0x200;
    constexpr u64 NEXT_LOAD_ARGV    = LOADER_PAGE + 0x800;   // envSetNextLoad: argv (0x800 bytes)
    constexpr u64 NEXT_LOAD_ARGV_SIZE = 0x800;
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
    ~Kernel() { StopCores(); for (auto& t : m_threads) t->wait_objects.clear(); }

    // Vuelve al estado inicial (sin heap, sin salida de texto).
    void Reset();

    // Prepara un proceso homebrew segun el "Homebrew ABI" (switchbrew.org/wiki/Homebrew_ABI):
    // crea pila, TLS y la lista de configuracion del loader, y pone los registros
    // de entrada (X0 = config, X1 = -1, SP = pila, X30 = stub que llama a svcExitProcess).
    void SetupHomebrewProcess(u64 entry, u64 image_base, u64 image_size, const std::string& argv);

    // Llamada desde el interprete al ejecutar "svc #imm" (con el candado del kernel cogido).
    void HandleSvc(u32 imm, Core::CPUState& state);
    // Punto de entrada de las CPU: coge el candado y llama a HandleSvc
    void SvcEntry(u32 imm, Core::CPUState& state);

    // --- Servicios que esperan al PC (red) ---
    // Sockets del proceso (bsd, ssl)
    NetworkState& Network();
    // Suelta el candado del kernel mientras se espera algo del PC (que los demas nucleos
    // sigan). Solo dentro de una SVC. Devuelve si lo solto (para volver a cogerlo despues).
    bool ReleaseLockForWait();
    void ReacquireLockAfterWait(bool released);
    // El emulador quiere parar los nucleos: las esperas largas deben terminar ya
    bool StopRequested() const { return m_coresStop.load(); }
    void HandleSvcImpl(u32 imm, Core::CPUState& state);

    // --- Hilos y planificador (kernel_threads.cpp) ---
    // Ejecuta hasta 'budget' instrucciones repartidas entre los hilos listos
    // (6 nucleos emulados que se turnan). Devuelve las instrucciones ejecutadas.
    u64 Run(u64 budget);
    const std::vector<std::shared_ptr<KThread>>& Threads() const { return m_threads; }
    const KThread* CurrentThread() const { return m_current.get(); }

    // --- Varios nucleos en hilos del PC (kernel_threads.cpp) ---
    // Run() ejecuta los 6 nucleos emulados por turnos en el hilo que llama: es
    // determinista y lo usan los tests. En modo multinucleo (la app) cada nucleo emulado
    // tiene su propio hilo del PC y corre a la vez que los demas:
    //   SetMulticore(true)  elegir el modo (con los nucleos parados)
    //   StartCores()        arrancar los hilos; StopCores() pararlos y esperarlos
    //   CoresHalted()       algun nucleo paro la CPU (fin, error): hay que llamar a StopCores()
    // El reloj (CNTPCT) pasa a ser la hora real. Toda SVC coge el candado del kernel (Lock()).
    void SetMulticore(bool on);
    bool IsMulticore() const { return m_multicore; }
    void StartCores();
    void StopCores();
    bool CoresRunning() const { return !m_coreThreads.empty(); }
    bool CoresHalted() const { return m_coresHalted.load(); }
    // Candado del kernel: la interfaz lo coge para leer hilos, salida, etc. con los nucleos en marcha
    std::unique_lock<std::recursive_mutex> Lock() { return std::unique_lock(m_lock); }
    // CPU de un nucleo (0 = la de System). nullptr si ese nucleo aun no ha hecho falta.
    Core::Interpreter* CoreCpu(s32 core) { return core == 0 ? &m_cpu : m_extraCpus[size_t(core)].get(); }

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
    // Lo que el programa pidio cargar al terminar (envSetNextLoad del hbmenu): ruta "sdmc:/..."
    // y argv. Vacio si no pidio nada.
    std::string GetNextLoadPath() { return ReadGuestString(Layout::NEXT_LOAD_PATH, Layout::NEXT_LOAD_PATH_SIZE); }
    std::string GetNextLoadArgv() { return ReadGuestString(Layout::NEXT_LOAD_ARGV, Layout::NEXT_LOAD_ARGV_SIZE); }
    std::string ReadGuestString(u64 addr, u64 max) {
        std::string out(max, '\0');
        m_memory.ReadBytes(addr, out.data(), max);
        out.resize(out.find('\0') == std::string::npos ? max : out.find('\0'));
        return out;
    }
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
    void SvcMapMemory(Core::CPUState& s, bool map);
    void SvcMapProcessCodeMemory(Core::CPUState& s, bool map);
    void SvcSetProcessMemoryPermission(Core::CPUState& s);

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
    u64  Ticks() { return Cpu().GetTicks(); }   // reloj del sistema (CNTPCT)
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
    u64  Now() { return Cpu().GetTicks(); }
    // El nucleo que esta ejecutando esta SVC: su CPU, su hilo y su numero. En el modo de
    // un hilo, la unica CPU (m_cpu) y el hilo cargado en ella (m_current).
    Core::Interpreter& Cpu();
    std::shared_ptr<KThread>& Cur();
    s32 CurCore() const;
    // Cambia un registro de un hilo que espera (puede estar aun cargado en otro nucleo)
    void SetReg(KThread& t, unsigned index, u64 value);
    void CoreLoop(s32 core);
    KThread* PickNextOnCore(s32 core);
    Core::Interpreter& EnsureCoreCpu(s32 core);
    static u64 WallClock(const void* kernel);
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
    std::shared_ptr<KThread> m_current;    // el hilo cuyos registros estan en la CPU (modo de un hilo)
    std::shared_ptr<NetworkState> m_network;

    // Modo multinucleo
    std::recursive_mutex m_lock;
    std::condition_variable_any m_coreCv;  // un hilo pasa a listo / hay que parar
    bool m_multicore = false;
    std::array<std::unique_ptr<Core::Interpreter>, NUM_CORES> m_extraCpus{};   // nucleos 1..5
    std::array<std::shared_ptr<KThread>, NUM_CORES> m_coreThread{};            // hilo cargado en cada nucleo
    std::vector<std::thread> m_coreThreads;
    std::atomic<bool> m_coresStop{false};
    std::atomic<bool> m_coresHalted{false};
    std::string m_coreHaltReason;
    // Reloj real: ticks = base + tiempo desde 'start' (solo cuenta con los nucleos en marcha)
    std::atomic<u64> m_clockBase{0};
    std::atomic<s64> m_clockStartNs{0};
    std::atomic<bool> m_clockRunning{false};
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
