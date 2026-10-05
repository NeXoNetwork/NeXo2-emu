#include "kernel.hpp"
#include "firmware.hpp"
#include "ipc.hpp"
#include "services/apm.hpp"
#include "services/applet.hpp"
#include "services/fs.hpp"
#include "services/hid.hpp"
#include "services/nvdrv.hpp"
#include "services/set.hpp"
#include "services/sm.hpp"
#include "services/time.hpp"
#include "services/vi.hpp"
#include "common/logger.hpp"
#include <cstdio>

namespace NeXo2::HLE {

using Common::Logger;
using Core::CPUState;
using Core::MemoryPermission;
using Core::MemoryState;

namespace {
// Escribe el resultado de una SVC en W0 (los 32 bits altos de X0 quedan a cero).
void SetResult(CPUState& s, u32 result) { s.x[0] = result; }

std::string Hex(u64 v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// Claves de la lista de configuracion del Homebrew ABI.
namespace ConfigKey {
    constexpr u32 EndOfList            = 0;
    constexpr u32 MainThreadHandle     = 1;
    constexpr u32 Argv                 = 5;
    constexpr u32 SyscallAvailableHint = 6;
    constexpr u32 AppletType           = 7;
    constexpr u32 HosVersion           = 16;
}
constexpr u32 CONFIG_FLAG_MANDATORY = 1;
} // namespace

Kernel::Kernel(Core::Memory& memory, Core::Interpreter& cpu)
    : m_memory(memory), m_cpu(cpu) {
    m_cpu.SetSvcHandler([this](u32 imm, CPUState& state) { HandleSvc(imm, state); });
    RegisterDefaultServices();
}

// Servicios que sm: sabe entregar. Cada servicio nuevo se anade aqui.
void Kernel::RegisterDefaultServices() {
    m_services.Register("set:sys",  [] { return std::make_shared<SystemSettings>(); });
    m_services.Register("apm",      [] { return std::make_shared<ApmManager>(); });
    m_services.Register("appletOE", [] { return std::make_shared<AppletOE>(); });
    m_services.Register("hid",      [] { return std::make_shared<HidServer>(); });
    m_services.Register("time:u",   [] { return std::make_shared<TimeService>("time:u"); });
    m_services.Register("time:a",   [] { return std::make_shared<TimeService>("time:a"); });
    m_services.Register("time:s",   [] { return std::make_shared<TimeService>("time:s"); });
    m_services.Register("fsp-srv",  [] { return std::make_shared<FileSystemProxy>(); });
    // Pantalla: vi (capas y cola de buffers) + nvdrv (memoria de la GPU)
    m_services.Register("vi:m",     [] { return std::make_shared<ViRoot>("vi:m", 2); });
    m_services.Register("vi:s",     [] { return std::make_shared<ViRoot>("vi:s", 1); });
    m_services.Register("vi:u",     [] { return std::make_shared<ViRoot>("vi:u", 0); });
    m_services.Register("nvdrv",    [] { return std::make_shared<NvDrv>("nvdrv"); });
    m_services.Register("nvdrv:a",  [] { return std::make_shared<NvDrv>("nvdrv:a"); });
    m_services.Register("nvdrv:s",  [] { return std::make_shared<NvDrv>("nvdrv:s"); });
}

void Kernel::Reset() {
    m_heapSize = 0;
    m_imageSize = 0;
    m_exited = false;
    m_debugOutput.clear();
    m_handles.Clear();
    m_display.Reset();
    m_input.Reset();
    m_hidMemory.reset();
    // Un hilo puede estar esperando a otro (o a si mismo): romper esos ciclos de punteros
    for (auto& t : m_threads) t->wait_objects.clear();
    m_threads.clear();
    m_current.reset();
    m_currentCore = 0;
    m_nextThreadId = 1;
    m_waitCounter = 0;
    m_runCounter = 0;
    m_freeTls.clear();
    m_tlsPagesUsed = 0;
    m_tlsNextSlot = 0;
    m_stats = {};
}

// ============================================================================
//  Preparacion del proceso (lo que haria el loader de homebrew "hbloader")
// ============================================================================

void Kernel::SetupHomebrewProcess(u64 entry, u64 image_base, u64 image_size, const std::string& argv) {
    using namespace Layout;
    (void)image_base;
    m_imageSize = image_size;

    // Pila del hilo principal (crece hacia abajo desde el final de la zona)
    m_memory.MapRegion(STACK_REGION_BASE, MAIN_STACK_SIZE, MemoryState::Stack,
                       MemoryPermission::ReadWrite, "pila principal");

    // TLS: 0x200 bytes por hilo. Horizon pone su direccion en TPIDRRO_EL0.
    // Los primeros 0x100 bytes son el buffer de mensajes IPC.
    m_memory.MapRegion(TLS_PAGE, Core::Memory::PAGE_SIZE, MemoryState::ThreadLocal,
                       MemoryPermission::ReadWrite, "TLS");

    // Pagina del loader:
    //   +0x000  stub de salida: si main() hace "ret", llega aqui y termina el proceso
    //   +0x100  texto informativo del loader
    //   +0x200  argv (linea de comandos)
    //   +0x400  lista de configuracion (Homebrew ABI)
    m_memory.MapRegion(LOADER_PAGE, Core::Memory::PAGE_SIZE, MemoryState::Static,
                       MemoryPermission::ReadExecute, "loader HLE");
    const u64 stub     = LOADER_PAGE;
    const u64 info_str = LOADER_PAGE + 0x100;
    const u64 argv_str = LOADER_PAGE + 0x200;
    const u64 config   = LOADER_PAGE + 0x400;

    m_memory.Write<u32>(stub + 0, 0xD40000E1u); // svc #0x7  (ExitProcess): main() ha vuelto
    m_memory.Write<u32>(stub + 4, 0xD4200000u); // brk #0    (no deberia llegar)
    m_memory.Write<u32>(stub + 8, 0xD4000141u); // svc #0xA  (ExitThread): la funcion de un hilo ha vuelto
    m_memory.Write<u32>(stub + 12, 0xD4200000u);

    const std::string info = "NeXo 2 HLE loader";
    m_memory.WriteBytes(info_str, info.c_str(), info.size() + 1);
    const std::string args = argv.substr(0, 0x1FF);
    m_memory.WriteBytes(argv_str, args.c_str(), args.size() + 1);

    // Cada entrada: u32 key, u32 flags, u64 value[2]  (0x18 bytes)
    u64 cursor = config;
    auto add_entry = [&](u32 key, u32 flags, u64 v0, u64 v1) {
        m_memory.Write<u32>(cursor + 0x0, key);
        m_memory.Write<u32>(cursor + 0x4, flags);
        m_memory.Write<u64>(cursor + 0x8, v0);
        m_memory.Write<u64>(cursor + 0x10, v1);
        cursor += 0x18;
    };
    add_entry(ConfigKey::MainThreadHandle, CONFIG_FLAG_MANDATORY, MAIN_THREAD_HANDLE, 0);
    add_entry(ConfigKey::AppletType, 0, 0 /* Application */, 0);
    add_entry(ConfigKey::Argv, 0, 0, argv_str);
    add_entry(ConfigKey::SyscallAvailableHint, 0, ~0ULL, ~0ULL);
    add_entry(ConfigKey::HosVersion, 0, Firmware::HOS_VERSION, 0);
    add_entry(ConfigKey::EndOfList, 0, info_str, info.size());

    // El hilo principal (sus registros se ponen justo debajo, en la CPU)
    CreateMainThread(TLS_PAGE);

    // Registros de entrada
    CPUState& s = m_cpu.GetState();
    s.x.fill(0);
    s.x[0]  = config;               // X0 = lista de configuracion
    s.x[1]  = ~0ULL;                // X1 = -1 -> "me ha cargado un loader de homebrew"
    s.x[30] = stub;                 // si el programa vuelve de su entrada, termina
    s.sp    = STACK_REGION_BASE + MAIN_STACK_SIZE;
    s.pc    = entry;
    s.tpidrro_el0 = TLS_PAGE;
    m_cpu.Resume();

    Logger::Log(Logger::Level::Info, "[HLE] Proceso homebrew listo. Entrada = " + Hex(entry));
}

// ============================================================================
//  Despachador de SVC
// ============================================================================

void Kernel::HandleSvc(u32 imm, CPUState& s) {
    switch (imm) {
        case 0x01: SvcSetHeapSize(s);       return;
        case 0x02: SvcSetMemoryPermission(s); return;
        case 0x03: // SetMemoryAttribute(addr, size, mask, valor): bloqueo de permisos / sin cache.
                   // No cambia nada en NeXo (no emulamos caches ni bloqueos): solo comprobamos alineacion.
            SetResult(s, (s.x[0] % Core::Memory::PAGE_SIZE || s.x[1] % Core::Memory::PAGE_SIZE)
                             ? Result::InvalidAddress : Result::Success);
            return;
        case 0x06: SvcQueryMemory(s);       return;
        case 0x07: SvcExitProcess(s);       return;
        case 0x08: SvcCreateThread(s);      return;
        case 0x09: SvcStartThread(s);       return;
        case 0x0A: SvcExitThread(s);        return;
        case 0x0B: SvcSleepThread(s);       return;
        case 0x0C: SvcGetThreadPriority(s); return;
        case 0x0D: SvcSetThreadPriority(s); return;
        case 0x0E: SvcGetThreadCoreMask(s); return;
        case 0x0F: SvcSetThreadCoreMask(s); return;
        case 0x10: s.x[0] = static_cast<u64>(m_currentCore); return; // GetCurrentProcessorNumber
        case 0x11: SvcSignalEvent(s);       return;
        case 0x12: SvcClearEvent(s);        return;
        case 0x13: SvcMapSharedMemory(s);   return;
        case 0x14: SvcUnmapSharedMemory(s); return;
        case 0x17: SvcClearEvent(s);        return; // ResetSignal: igual que ClearEvent aqui
        case 0x18: SvcWaitSynchronization(s); return;
        case 0x24: SetResult(s, Result::Success); s.x[1] = 0x51; return; // GetProcessId
        case 0x25: SvcGetThreadId(s);       return;
        case 0x19: SvcCancelSynchronization(s); return;
        case 0x15: {
            // CreateTransferMemory(addr = X1, size = X2, perm = W3) -> W1 = handle.
            // La memoria sigue en el proceso; el servicio (nvdrv) solo guarda el handle.
            if (s.x[1] % Core::Memory::PAGE_SIZE || s.x[2] % Core::Memory::PAGE_SIZE || s.x[2] == 0) {
                SetResult(s, Result::InvalidAddress); return;
            }
            s.x[1] = m_handles.Create(std::make_shared<KDummyObject>("transfer memory"));
            SetResult(s, Result::Success);
            return;
        }
        case 0x16: SvcCloseHandle(s);       return;
        // --- Mutex y variables de condicion (NeXo solo tiene un hilo) ---
        // --- Mutex y variables de condicion (kernel_threads.cpp) ---
        case 0x1A: SvcArbitrateLock(s);     return;
        case 0x1B: SvcArbitrateUnlock(s);   return;
        case 0x1C: SvcWaitProcessWideKeyAtomic(s); return;
        case 0x1D: SvcSignalProcessWideKey(s); return;
        case 0x1E: SvcGetSystemTick(s);     return;
        case 0x26: SvcBreak(s);             return;
        case 0x27: SvcOutputDebugString(s); return;
        case 0x1F: SvcConnectToNamedPort(s); return;
        case 0x21: SvcSendSyncRequest(s);    return;
        case 0x22: SvcSendSyncRequestWithUserBuffer(s); return;
        case 0x29: SvcGetInfo(s);           return;
        default: {
            // SVC desconocida: paramos para que se vea claramente que falta.
            char buf[96];
            std::snprintf(buf, sizeof(buf), "SVC 0x%02X (%s) no implementada", imm, SvcName(imm));
            Logger::Log(Logger::Level::Warning, std::string("[HLE] ") + buf);
            m_cpu.Halt(buf);
            return;
        }
    }
}

// svcSetHeapSize(size) -> W0 = resultado, X1 = direccion del heap
void Kernel::SvcSetHeapSize(CPUState& s) {
    const u64 size = s.x[1];
    if (size % 0x200000 != 0)                 { SetResult(s, Result::InvalidSize); return; }
    if (size > Layout::HEAP_REGION_SIZE)      { SetResult(s, Result::OutOfMemory); return; }

    m_memory.UnmapRegion(Layout::HEAP_REGION_BASE, Layout::HEAP_REGION_SIZE);
    if (size > 0)
        m_memory.MapRegion(Layout::HEAP_REGION_BASE, size, MemoryState::Normal,
                           MemoryPermission::ReadWrite, "heap");
    m_heapSize = size;
    SetResult(s, Result::Success);
    s.x[1] = Layout::HEAP_REGION_BASE;
}

// svcSetMemoryPermission(addr = X0, size = X1, permisos = W2)
// libnx lo usa al arrancar para dejar de solo lectura la zona que acaba de recolocar (RELRO).
void Kernel::SvcSetMemoryPermission(CPUState& s) {
    const u64 addr = s.x[0], size = s.x[1];
    const u32 perm = static_cast<u32>(s.x[2]);
    if (addr % Core::Memory::PAGE_SIZE || size % Core::Memory::PAGE_SIZE || size == 0) {
        SetResult(s, Result::InvalidAddress);
        return;
    }
    if (perm != 0 && perm != 1 && perm != 3) { SetResult(s, 0xD801); return; } // InvalidNewMemoryPermission
    const Core::MemoryRegion r = m_memory.QueryRegion(addr);
    if (r.state == MemoryState::Free || addr + size > r.End()) { SetResult(s, Result::InvalidState); return; }
    m_memory.MapRegion(addr, size, r.state, static_cast<MemoryPermission>(perm), r.name);
    SetResult(s, Result::Success);
}

// svcQueryMemory(MemoryInfo* out = X0, addr = X2) -> W0 = resultado, W1 = PageInfo
void Kernel::SvcQueryMemory(CPUState& s) {
    const u64 out  = s.x[0];
    const u64 addr = s.x[2];
    const Core::MemoryRegion r = m_memory.QueryRegion(addr);

    // struct MemoryInfo (0x28 bytes)
    m_memory.Write<u64>(out + 0x00, r.base);
    m_memory.Write<u64>(out + 0x08, r.size);
    m_memory.Write<u32>(out + 0x10, static_cast<u32>(r.state));
    m_memory.Write<u32>(out + 0x14, 0); // atributos
    m_memory.Write<u32>(out + 0x18, static_cast<u32>(r.perm));
    m_memory.Write<u32>(out + 0x1C, 0); // ipc_refcount
    m_memory.Write<u32>(out + 0x20, 0); // device_refcount
    m_memory.Write<u32>(out + 0x24, 0); // relleno

    SetResult(s, Result::Success);
    s.x[1] = 0; // PageInfo
}

void Kernel::SvcExitProcess(CPUState&) {
    m_exited = true;
    m_cpu.Halt("svcExitProcess: el programa ha terminado");
}

// svcSleepThread(ns = X0). 0, -1 y -2 significan "cede el turno" (yield).
void Kernel::SvcSleepThread(CPUState& s) {
    const s64 ns = static_cast<s64>(s.x[0]);
    if (!m_current) return;                       // programa sin proceso: nada que hacer
    if (ns <= 0) { m_cpu.RequestStop(); return; } // yield: el planificador elige otro
    Block(*m_current, KThread::Wait::Sleep, ns);
}

// svcCloseHandle(handle = W0)
void Kernel::SvcCloseHandle(CPUState& s) {
    const u32 handle = static_cast<u32>(s.x[0]);
    if (handle == CURRENT_THREAD_PSEUDO_HANDLE || handle == CURRENT_PROCESS_PSEUDO_HANDLE) {
        SetResult(s, Result::Success);
        return;
    }
    SetResult(s, m_handles.Close(handle) ? Result::Success : Result::InvalidHandle);
}

// svcGetSystemTick -> X0 = ticks. Mismo contador que CNTPCT_EL0.
void Kernel::SvcGetSystemTick(CPUState& s) {
    s.x[0] = m_cpu.GetInstructionCount();
}

// svcBreak(reason = X0, arg = X1, size = X2): el programa ha fallado (abort, assert...).
void Kernel::SvcBreak(CPUState& s) {
    m_cpu.Halt("svcBreak (razon " + Hex(s.x[0]) + "): el programa ha abortado");
}

// svcOutputDebugString(str = X0, size = X1)
void Kernel::SvcOutputDebugString(CPUState& s) {
    const u64 size = s.x[1] > 0x10000 ? 0x10000 : s.x[1];
    std::string text(size, '\0');
    m_memory.ReadBytes(s.x[0], text.data(), size);
    m_debugOutput += text;
    if (!text.empty() && text.back() != '\n') m_debugOutput += '\n';

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    Logger::Log(Logger::Level::Info, "[Programa] " + text);
    SetResult(s, Result::Success);
}

// svcGetInfo(id0 = W1, handle = W2, id1 = X3) -> W0 = resultado, X1 = valor
void Kernel::SvcGetInfo(CPUState& s) {
    using namespace Layout;
    const u32 id0 = static_cast<u32>(s.x[1]);
    const u64 id1 = s.x[3];
    u64 value;
    switch (id0) {
        case 0:  value = 0x3F;  break;                         // CoreMask: nucleos 0-5 (Switch 2)
        case 1:  value = ~0ULL; break;                         // PriorityMask
        case 2:  value = ALIAS_REGION_BASE; break;
        case 3:  value = ALIAS_REGION_SIZE; break;
        case 4:  value = HEAP_REGION_BASE;  break;
        case 5:  value = HEAP_REGION_SIZE;  break;
        case 6:  value = TOTAL_MEMORY;      break;             // TotalMemorySize
        case 7:  value = m_imageSize + MAIN_STACK_SIZE + m_heapSize; break; // UsedMemorySize
        case 8:  value = 0; break;                             // DebuggerAttached
        case 11: value = 0x4E65586F32ULL ^ (id1 * 0x9E3779B97F4A7C15ULL); break; // RandomEntropy (fijo)
        case 12: value = ASLR_REGION_BASE;  break;
        case 13: value = ASLR_REGION_SIZE;  break;
        case 14: value = STACK_REGION_BASE; break;
        case 15: value = STACK_REGION_SIZE; break;
        case 16: case 17: value = 0; break;                    // SystemResourceSize total/usado
        case 18: value = 0x0100000000001000ULL; break;         // ProgramId (inventado)
        case 21: value = TOTAL_MEMORY; break;                  // TotalNonSystemMemorySize
        case 22: value = m_imageSize + MAIN_STACK_SIZE + m_heapSize; break;
        case 23: value = 1; break;                             // IsApplication
        case 28: value = 0; break;                             // AliasRegionExtraSize [18.0.0+]
        default:
            Logger::Log(Logger::Level::Warning, "[HLE] svcGetInfo: id " + std::to_string(id0) + " no implementado");
            SetResult(s, Result::InvalidEnumValue);
            return;
    }
    SetResult(s, Result::Success);
    s.x[1] = value;
}

// ============================================================================

const char* Kernel::SvcName(u32 imm) {
    static const char* const names[0x80] = {
        "?", "SetHeapSize", "SetMemoryPermission", "SetMemoryAttribute", "MapMemory", "UnmapMemory",
        "QueryMemory", "ExitProcess", "CreateThread", "StartThread", "ExitThread", "SleepThread",
        "GetThreadPriority", "SetThreadPriority", "GetThreadCoreMask", "SetThreadCoreMask",
        "GetCurrentProcessorNumber", "SignalEvent", "ClearEvent", "MapSharedMemory", "UnmapSharedMemory",
        "CreateTransferMemory", "CloseHandle", "ResetSignal", "WaitSynchronization", "CancelSynchronization",
        "ArbitrateLock", "ArbitrateUnlock", "WaitProcessWideKeyAtomic", "SignalProcessWideKey",
        "GetSystemTick", "ConnectToNamedPort", "SendSyncRequestLight", "SendSyncRequest",
        "SendSyncRequestWithUserBuffer", "SendAsyncRequestWithUserBuffer", "GetProcessId", "GetThreadId",
        "Break", "OutputDebugString", "ReturnFromException", "GetInfo", "FlushEntireDataCache",
        "FlushDataCache", "MapPhysicalMemory", "UnmapPhysicalMemory", "GetDebugFutureThreadInfo",
        "GetLastThreadInfo", "GetResourceLimitLimitValue", "GetResourceLimitCurrentValue",
        "SetThreadActivity", "GetThreadContext3", "WaitForAddress", "SignalToAddress",
        "SynchronizePreemptionState", "GetResourceLimitPeakValue", "?", "CreateIoPool", "CreateIoRegion",
        "?", "KernelDebug", "ChangeKernelTraceState", "?", "?", "CreateSession", "AcceptSession",
        "ReplyAndReceiveLight", "ReplyAndReceive", "ReplyAndReceiveWithUserBuffer", "CreateEvent",
        "MapIoRegion", "UnmapIoRegion", "MapPhysicalMemoryUnsafe", "UnmapPhysicalMemoryUnsafe",
        "SetUnsafeLimit", "CreateCodeMemory", "ControlCodeMemory", "SleepSystem", "ReadWriteRegister",
        "SetProcessActivity", "CreateSharedMemory", "MapTransferMemory", "UnmapTransferMemory",
        "CreateInterruptEvent", "QueryPhysicalAddress", "QueryIoMapping", "CreateDeviceAddressSpace",
        "AttachDeviceAddressSpace", "DetachDeviceAddressSpace", "MapDeviceAddressSpaceByForce",
        "MapDeviceAddressSpaceAligned", "MapDeviceAddressSpace", "UnmapDeviceAddressSpace",
        "InvalidateProcessDataCache", "StoreProcessDataCache", "FlushProcessDataCache",
        "DebugActiveProcess", "BreakDebugProcess", "TerminateDebugProcess", "GetDebugEvent",
        "ContinueDebugEvent", "GetProcessList", "GetThreadList", "GetDebugThreadContext",
        "SetDebugThreadContext", "QueryDebugProcessMemory", "ReadDebugProcessMemory",
        "WriteDebugProcessMemory", "SetHardwareBreakPoint", "GetDebugThreadParam", "?", "GetSystemInfo",
        "CreatePort", "ManageNamedPort", "ConnectToPort", "SetProcessMemoryPermission", "MapProcessMemory",
        "UnmapProcessMemory", "QueryProcessMemory", "MapProcessCodeMemory", "UnmapProcessCodeMemory",
        "CreateProcess", "StartProcess", "TerminateProcess", "GetProcessInfo", "CreateResourceLimit",
        "SetResourceLimitLimitValue", "CallSecureMonitor",
    };
    return imm < 0x80 ? names[imm] : "?";
}

// ============================================================================
//  IPC: conexion a puertos y envio de mensajes a servicios
// ============================================================================

u32 Kernel::CreateSessionHandle(std::shared_ptr<ServiceObject> service) {
    auto state = std::make_shared<SessionState>();
    state->root = std::move(service);
    return m_handles.Create(std::make_shared<KClientSession>(state));
}

void Kernel::HaltWithMessage(const std::string& message) {
    Logger::Log(Logger::Level::Warning, "[HLE] " + message);
    m_cpu.Halt(message);
}

void Kernel::ReportUnimplemented(const std::string& service_name, u32 command_id) {
    const std::string msg = "Servicio '" + service_name + "': comando " + std::to_string(command_id) +
                            " no implementado";
    Logger::Log(Logger::Level::Warning, "[HLE] " + msg);
    m_cpu.Halt(msg);
}

// svcConnectToNamedPort(nombre = X1) -> W0 = resultado, W1 = handle
// El unico puerto con nombre que usan los programas es "sm:".
void Kernel::SvcConnectToNamedPort(CPUState& s) {
    char name[13] = {};
    m_memory.ReadBytes(s.x[1], name, 12);
    const std::string port(name);

    if (port != "sm:") {
        Logger::Log(Logger::Level::Warning, "[HLE] svcConnectToNamedPort(\"" + port + "\"): puerto desconocido");
        SetResult(s, Result::NotFound);
        return;
    }
    const u32 handle = CreateSessionHandle(std::make_shared<ServiceManager>());
    Logger::Log(Logger::Level::Info, "[HLE] svcConnectToNamedPort(\"sm:\") -> handle " + std::to_string(handle));
    SetResult(s, Result::Success);
    s.x[1] = handle;
}

// svcSendSyncRequest(handle = W0): el mensaje esta en la TLS del hilo
void Kernel::SvcSendSyncRequest(CPUState& s) {
    SetResult(s, ProcessIpcRequest(static_cast<u32>(s.x[0]), s.tpidrro_el0));
}

// svcSendSyncRequestWithUserBuffer(buffer = X0, tamano = X1, handle = W2): el mensaje esta en 'buffer'
void Kernel::SvcSendSyncRequestWithUserBuffer(CPUState& s) {
    SetResult(s, ProcessIpcRequest(static_cast<u32>(s.x[2]), s.x[0]));
}

u32 Kernel::ProcessIpcRequest(u32 handle, u64 message) {
    auto session = m_handles.Get<KClientSession>(handle);
    if (!session) {
        Logger::Log(Logger::Level::Warning, "[IPC] Handle " + std::to_string(handle) + " no es una sesion");
        return Result::InvalidHandle;
    }
    const std::shared_ptr<SessionState>& state = session->state;
    IpcRequest req = ParseHipcRequest(m_memory, message);

    // --- Cerrar la sesion (despues el programa hara svcCloseHandle) ---
    if (req.type == CommandType::Close) return Result::Success;

    // --- TIPC: type = 16 + comando, los argumentos empiezan directamente en los datos ---
    if (req.type >= CommandType::TipcBase) {
        req.command_id = req.type - CommandType::TipcBase;
        req.payload_offset = message + req.data_offset;
        req.payload_size = req.data_size;
        IpcContext ctx(*this, m_memory, req, IpcContext::Protocol::Tipc, false);
        if (!state->root->SupportsTipc()) {
            ctx.Unimplemented(state->root->Name() + " (TIPC)");
            return Result::Success;
        }
        state->root->Dispatch(ctx);
        if (ctx.IsHandled()) ctx.WriteResponse(message, state);
        return Result::Success;
    }

    const bool is_request = req.type == CommandType::Request || req.type == CommandType::RequestWithContext;
    const bool is_control = req.type == CommandType::Control || req.type == CommandType::ControlWithContext;
    if (!is_request && !is_control) {
        m_cpu.Halt("Mensaje IPC de tipo " + std::to_string(req.type) + " no soportado");
        return Result::Success;
    }

    // --- CMIF: los datos empiezan alineados a 16 bytes ---
    u64 cursor = message + ((req.data_offset + 15) & ~u64(15));
    const u64 data_end = message + req.data_offset + req.data_size;
    std::shared_ptr<ServiceObject> target = state->root;
    const bool domain_message = is_request && state->is_domain;

    if (domain_message) {
        // Cabecera de dominio: tipo, n objetos de entrada, tamano, id del objeto destino
        const u8  dtype = m_memory.Read<u8>(cursor + 0);
        const u8  num_in_objects = m_memory.Read<u8>(cursor + 1);
        const u16 dsize = m_memory.Read<u16>(cursor + 2);
        const u32 object_id = m_memory.Read<u32>(cursor + 4);
        req.is_domain_message = true;
        req.domain_command = dtype;
        req.domain_object_id = object_id;
        for (u32 i = 0; i < num_in_objects; ++i)
            req.domain_in_objects.push_back(m_memory.Read<u32>(cursor + 16 + dsize + i * 4));

        if (dtype == 2) { // cerrar un objeto del dominio
            state->domain_objects.erase(object_id);
            IpcContext ctx(*this, m_memory, req, IpcContext::Protocol::Cmif, true);
            ctx.WriteResponse(message, state);
            return Result::Success;
        }
        auto it = state->domain_objects.find(object_id);
        if (it == state->domain_objects.end()) {
            m_cpu.Halt("IPC: objeto de dominio " + std::to_string(object_id) + " no existe");
            return Result::Success;
        }
        target = it->second;
        cursor += 16;
    }

    if (m_memory.Read<u32>(cursor) != CMIF_IN_MAGIC)
        Logger::Log(Logger::Level::Warning, "[IPC] Falta la cabecera SFCI en el mensaje");
    req.command_id = m_memory.Read<u32>(cursor + 8);
    req.payload_offset = cursor + 16;
    req.payload_size = data_end > cursor + 16 ? static_cast<u32>(data_end - (cursor + 16)) : 0;

    IpcContext ctx(*this, m_memory, req, IpcContext::Protocol::Cmif, domain_message);
    if (is_control) HandleControlCommand(ctx, state);
    else            target->Dispatch(ctx);
    if (ctx.IsHandled()) ctx.WriteResponse(message, state);
    return Result::Success;
}

void Kernel::HandleControlCommand(IpcContext& ctx, const std::shared_ptr<SessionState>& state) {
    const std::string& name = state->root->Name();
    switch (ctx.CommandId()) {
        case 0: { // ConvertCurrentObjectToDomain -> id del objeto principal
            if (!state->is_domain) {
                state->is_domain = true;
                state->AddDomainObject(state->root);
            }
            const u32 id = state->domain_objects.begin()->first;
            Logger::Log(Logger::Level::Info, "[IPC] " + name + " convertido a dominio (objeto " + std::to_string(id) + ")");
            ctx.Push<u32>(id);
            break;
        }
        case 1: { // CopyFromCurrentDomain(id) -> sesion nueva con ese objeto
            const u32 id = ctx.Pop<u32>();
            auto it = state->domain_objects.find(id);
            if (it == state->domain_objects.end()) { ctx.SetResult(Result::InvalidHandle); return; }
            ctx.PushMoveHandle(CreateSessionHandle(it->second));
            break;
        }
        case 2:   // CloneCurrentObject
        case 4: { // CloneCurrentObjectEx(tag)
            ctx.PushMoveHandle(m_handles.Create(std::make_shared<KClientSession>(state)));
            break;
        }
        case 3:   // QueryPointerBufferSize -> tamano del buffer para punteros (descriptores X/C)
            ctx.Push<u16>(0x8000);
            break;
        default:
            ctx.Unimplemented(name + " (control)");
            return;
    }
    ctx.SetResult(Result::Success);
}

// ============================================================================
//  Eventos y memoria compartida
// ============================================================================

u32 Kernel::CreateEvent(const std::string& name, bool signaled) {
    return m_handles.Create(std::make_shared<KEvent>(name, signaled));
}

u32 Kernel::CreateSharedMemory(const std::string& name, size_t size, std::shared_ptr<KSharedMemory>* out) {
    auto shmem = std::make_shared<KSharedMemory>(name, size);
    if (out) *out = shmem;
    return m_handles.Create(shmem);
}

u32 Kernel::GetHidSharedMemoryHandle() {
    if (!m_hidMemory) {
        m_hidMemory = std::make_shared<KSharedMemory>("hid", HidLayout::SIZE);
        m_input.InitSharedMemory(m_hidMemory->data);
    }
    return m_handles.Create(m_hidMemory);
}

void Kernel::SetPadInput(const PadInput& pad) {
    if (!m_hidMemory) return;   // el programa aun no ha pedido la memoria de hid
    m_input.Update(m_hidMemory->data, m_memory, m_hidMemory->mapped_address, pad);
}

// svcClearEvent / svcResetSignal(handle = W0)
void Kernel::SvcClearEvent(CPUState& s) {
    auto ev = m_handles.Get<KEvent>(static_cast<u32>(s.x[0]));
    if (!ev) { SetResult(s, Result::InvalidHandle); return; }
    ev->signaled = false;
    SetResult(s, Result::Success);
}

// svcMapSharedMemory(handle = W0, addr = X1, size = X2, permisos = W3)
void Kernel::SvcMapSharedMemory(CPUState& s) {
    auto shmem = m_handles.Get<KSharedMemory>(static_cast<u32>(s.x[0]));
    const u64 addr = s.x[1], size = s.x[2];
    if (!shmem) { SetResult(s, Result::InvalidHandle); return; }
    if (addr % Core::Memory::PAGE_SIZE || size % Core::Memory::PAGE_SIZE) { SetResult(s, Result::InvalidAddress); return; }
    if (size > shmem->data.size()) { SetResult(s, Result::InvalidSize); return; }
    if (m_memory.QueryRegion(addr).state != MemoryState::Free) { SetResult(s, Result::InvalidState); return; }

    m_memory.WriteBytes(addr, shmem->data.data(), size);
    shmem->mapped_address = addr;
    const auto perm = (s.x[3] & 2) ? MemoryPermission::ReadWrite : MemoryPermission::Read;
    m_memory.MapRegion(addr, size, MemoryState::Shared, perm, "compartida: " + shmem->name);
    Logger::Log(Logger::Level::Info, "[HLE] svcMapSharedMemory(" + shmem->name + ") en " + Hex(addr));
    SetResult(s, Result::Success);
}

// svcUnmapSharedMemory(handle = W0, addr = X1, size = X2)
void Kernel::SvcUnmapSharedMemory(CPUState& s) {
    if (auto shmem = m_handles.Get<KSharedMemory>(static_cast<u32>(s.x[0]))) shmem->mapped_address = 0;
    m_memory.UnmapRegion(s.x[1], s.x[2]);
    SetResult(s, Result::Success);
}

} // namespace NeXo2::HLE
