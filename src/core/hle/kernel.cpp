#include "kernel.hpp"
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
}
constexpr u32 CONFIG_FLAG_MANDATORY = 1;
} // namespace

Kernel::Kernel(Core::Memory& memory, Core::Interpreter& cpu)
    : m_memory(memory), m_cpu(cpu) {
    m_cpu.SetSvcHandler([this](u32 imm, CPUState& state) { HandleSvc(imm, state); });
}

void Kernel::Reset() {
    m_heapSize = 0;
    m_imageSize = 0;
    m_exited = false;
    m_debugOutput.clear();
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

    m_memory.Write<u32>(stub + 0, 0xD40000E1u); // svc #0x7  (ExitProcess)
    m_memory.Write<u32>(stub + 4, 0xD4200000u); // brk #0    (no deberia llegar)

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
    add_entry(ConfigKey::EndOfList, 0, info_str, info.size());

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
        case 0x06: SvcQueryMemory(s);       return;
        case 0x07: SvcExitProcess(s);       return;
        case 0x0B: SvcSleepThread(s);       return;
        case 0x16: SvcCloseHandle(s);       return;
        case 0x1E: SvcGetSystemTick(s);     return;
        case 0x26: SvcBreak(s);             return;
        case 0x27: SvcOutputDebugString(s); return;
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

void Kernel::SvcSleepThread(CPUState&) {
    // Con un solo hilo no hay nada que hacer: seguimos ejecutando.
}

void Kernel::SvcCloseHandle(CPUState& s) {
    SetResult(s, Result::Success);
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

} // namespace NeXo2::HLE
