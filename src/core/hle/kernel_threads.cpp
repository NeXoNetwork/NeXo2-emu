// Hilos, planificador y sincronizacion del kernel HLE.
//
// Dos modos:
//   - Run(): 6 nucleos emulados que se turnan en UN hilo del PC (determinista; tests).
//   - Multinucleo (StartCores): cada nucleo emulado en su propio hilo del PC, con su
//     propia CPU. Comparten la memoria y el kernel (un candado: m_lock). Ver al final.
//
// MODELO de un hilo:
//   - Cada hilo del programa es un KThread con sus registros guardados (ctx).
//   - Kernel::Run() reparte "franjas" de SCHEDULER_SLICE instrucciones: va rotando
//     por los nucleos y en cada uno ejecuta el hilo listo de mayor prioridad
//     (a igual prioridad, el que lleva mas tiempo sin correr).
//   - Una SVC que bloquea (esperar un evento, un mutex, dormir...) marca el hilo
//     como Waiting y pide a la CPU que pare (RequestStop) para cambiar de hilo.
//   - Si todos los hilos duermen con plazo, se adelanta el reloj hasta el primero.
//     Si todos esperan algo que nunca llegara, es un bloqueo: se para con un mensaje.
//
// Tiempo: el reloj del sistema es el contador de instrucciones (1 instruccion = 1 tick
// a 31,25 MHz, ver Interpreter::TICK_FREQUENCY).
//
// Mutex y variables de condicion siguen el protocolo de Horizon que usa libnx:
//   - La palabra del mutex vale 0 (libre) o el handle del dueno, con el bit
//     MUTEX_HAS_WAITERS si alguien espera.
//   - svcArbitrateLock: "espero a que el dueno lo suelte".
//   - svcArbitrateUnlock: el dueno lo suelta; el kernel se lo da al siguiente.
//   - svcWaitProcessWideKeyAtomic: suelta el mutex y espera una senal en 'key';
//     al despertar vuelve a coger el mutex antes de seguir.
//   - svcSignalProcessWideKey: despierta a 'count' hilos de 'key'.
// Referencia: switchbrew.org/wiki/SVC y docs/07-nexo-internals/threads.md
#include "kernel.hpp"
#include "arm64/fp_ops.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>

namespace NeXo2::HLE {

using Common::Logger;
using Core::CPUState;
using Core::MemoryPermission;
using Core::MemoryState;

namespace {
constexpr u64 NEVER = ~0ull;
void SetResult(CPUState& s, u32 result) { s.x[0] = result; }
// Nucleo emulado que corre en este hilo del PC (-1 = no es un hilo de nucleo)
thread_local s32 t_core = -1;
} // namespace

Core::Interpreter& Kernel::Cpu() { return t_core >= 0 ? *CoreCpu(t_core) : m_cpu; }
std::shared_ptr<KThread>& Kernel::Cur() { return t_core >= 0 ? m_coreThread[size_t(t_core)] : m_current; }
s32 Kernel::CurCore() const { return t_core >= 0 ? t_core : m_currentCore; }

void Kernel::SetReg(KThread& t, unsigned index, u64 value) {
    if (t.on_core >= 0 && &t != Cur().get()) {
        // Cargado en otro nucleo que lo esta descargando: se aplica al guardar sus registros
        t.pending_mask |= u8(1u << index);
        t.pending_x[index] = value;
        return;
    }
    Ctx(t).x[index] = value;
}

u64 Kernel::NsToTicks(s64 ns) {
    if (ns <= 0) return 0;
    // 31,25 MHz -> 1 tick = 32 ns
    return static_cast<u64>(ns) / 32 + 1;
}

CPUState& Kernel::Ctx(KThread& t) {
    return (&t == Cur().get()) ? Cpu().GetState() : t.ctx;
}

std::shared_ptr<KThread> Kernel::ThreadFromHandle(u32 handle) {
    if (handle == CURRENT_THREAD_PSEUDO_HANDLE) return Cur();
    return m_handles.Get<KThread>(handle);
}

// ============================================================================
//  TLS: 0x200 bytes por hilo. El principal usa la primera de TLS_PAGE; el resto
//  van en paginas hacia abajo, que se mapean cuando hacen falta.
// ============================================================================

u64 Kernel::AllocateTls() {
    using namespace Layout;
    u64 tls;
    if (!m_freeTls.empty()) {
        tls = m_freeTls.back();
        m_freeTls.pop_back();
    } else {
        const u64 slots_per_page = Core::Memory::PAGE_SIZE / TLS_SLOT_SIZE;
        const u64 page = m_tlsNextSlot / slots_per_page;
        if (page >= TLS_MAX_PAGES) return 0;
        const u64 page_addr = TLS_PAGE - page * Core::Memory::PAGE_SIZE;
        if (page >= m_tlsPagesUsed) {
            m_memory.MapRegion(page_addr, Core::Memory::PAGE_SIZE, MemoryState::ThreadLocal,
                               MemoryPermission::ReadWrite, "TLS");
            m_tlsPagesUsed = page + 1;
        }
        tls = page_addr + (m_tlsNextSlot % slots_per_page) * TLS_SLOT_SIZE;
        ++m_tlsNextSlot;
    }
    static const u8 zeros[Layout::TLS_SLOT_SIZE] = {};
    m_memory.WriteBytes(tls, zeros, sizeof(zeros));
    return tls;
}

void Kernel::FreeTls(u64 tls) {
    if (tls != 0) m_freeTls.push_back(tls);
}

std::shared_ptr<KThread> Kernel::CreateMainThread(u64 tls) {
    auto t = std::make_shared<KThread>();
    t->id = m_nextThreadId++;
    t->handle = MAIN_THREAD_HANDLE;
    t->priority = 44;
    t->core = 0;
    t->affinity = 1;
    t->tls = tls;
    t->state = KThread::State::Ready;
    t->name = "principal";
    m_threads.push_back(t);
    m_current = t;                 // sus registros ya estan en la CPU
    m_currentCore = 0;
    m_tlsNextSlot = 1;             // el hueco 0 de TLS_PAGE es suyo
    m_tlsPagesUsed = 1;
    m_handles.Insert(MAIN_THREAD_HANDLE, t);
    return t;
}

// ============================================================================
//  Espera y despertar
// ============================================================================

void Kernel::Block(KThread& t, KThread::Wait kind, s64 timeout_ns) {
    t.state = KThread::State::Waiting;
    t.wait = kind;
    t.wait_order = ++m_waitCounter;
    t.deadline = (timeout_ns < 0) ? NEVER : Now() + NsToTicks(timeout_ns);
    if (&t == Cur().get()) Cpu().RequestStop();   // cambiar de hilo tras esta SVC
}

void Kernel::SleepCurrentUntil(u64 deadline) {
    if (!Cur() || deadline <= Now()) return;
    KThread& t = *Cur();
    t.state = KThread::State::Waiting;
    t.wait = KThread::Wait::Sleep;      // al despertar no se cambia X0 (el resultado del servicio)
    t.wait_order = ++m_waitCounter;
    t.deadline = deadline;
    Cpu().RequestStop();
}

void Kernel::Wake(KThread& t, u32 result) {
    SetReg(t, 0, result);
    t.state = KThread::State::Ready;
    t.wait = KThread::Wait::None;
    t.wait_objects.clear();
    t.deadline = NEVER;
    if (m_multicore) m_coreCv.notify_all();   // su nucleo puede estar esperando trabajo
}

// Da el mutex de 'addr' al hilo de mayor prioridad que lo espera, o lo deja libre.
void Kernel::ReleaseMutex(u64 addr) {
    KThread* best = nullptr;
    int waiters = 0;
    for (auto& t : m_threads) {
        if (t->state != KThread::State::Waiting || t->wait != KThread::Wait::Mutex || t->mutex_addr != addr)
            continue;
        ++waiters;
        if (!best || t->priority < best->priority ||
            (t->priority == best->priority && t->wait_order < best->wait_order))
            best = t.get();
    }
    if (!best) {
        m_memory.Write<u32>(addr, 0);
        return;
    }
    m_memory.Write<u32>(addr, best->mutex_tag | (waiters > 1 ? MUTEX_HAS_WAITERS : 0));
    Wake(*best, best->pending_result);
}

// Tras una senal (o plazo vencido) de una variable de condicion, el hilo tiene que
// recuperar su mutex: si esta libre lo coge ya; si no, se pone a esperarlo.
void Kernel::AcquireMutexAfterWait(KThread& t, u32 result) {
    const u64 addr = t.mutex_addr;
    t.cv_key = 0;
    // Con compare-and-swap: con varios nucleos, otro hilo puede estar cogiendo el mutex
    // desde el programa (sin entrar en el kernel) justo ahora.
    u32 value = m_memory.Read<u32>(addr);
    for (;;) {
        if (value == 0) {
            if (!m_memory.CompareExchange<u32>(addr, value, t.mutex_tag)) continue;
            Wake(t, result);
            return;
        }
        if ((value & MUTEX_HAS_WAITERS) || m_memory.CompareExchange<u32>(addr, value, value | MUTEX_HAS_WAITERS)) break;
    }
    t.wait = KThread::Wait::Mutex;
    t.pending_result = result;
    t.deadline = NEVER;
    t.wait_order = ++m_waitCounter;
}

void Kernel::UpdateWaits() {
    // Avisos de la GPU (syncpoints que llegaron en su hilo): activan eventos de nvdrv
    m_gpu.GetSyncpoints().RunFired();
    const u64 now = Now();
    for (auto& tp : m_threads) {
        KThread& t = *tp;
        if (t.state != KThread::State::Waiting) continue;
        switch (t.wait) {
            case KThread::Wait::Sync: {
                for (size_t i = 0; i < t.wait_objects.size(); ++i) {
                    if (t.wait_objects[i]->IsSignaled()) {
                        Wake(t, Result::Success);
                        SetReg(t, 1, i);
                        break;
                    }
                }
                if (t.state == KThread::State::Waiting && t.deadline <= now) Wake(t, Result::TimedOut);
                break;
            }
            case KThread::Wait::Sleep:
                if (t.deadline <= now) {
                    t.state = KThread::State::Ready;
                    t.wait = KThread::Wait::None;
                    t.deadline = NEVER;
                    if (m_multicore) m_coreCv.notify_all();
                }
                break;
            case KThread::Wait::CondVar:
                if (t.deadline <= now) AcquireMutexAfterWait(t, Result::TimedOut);
                break;
            default:
                break;   // Mutex: solo lo despierta ArbitrateUnlock
        }
    }
}

// ============================================================================
//  Planificador
// ============================================================================

KThread* Kernel::PickNext() {
    // Rotar por los nucleos empezando por el siguiente al ultimo usado
    for (s32 i = 1; i <= NUM_CORES; ++i) {
        const s32 core = (m_currentCore + i) % NUM_CORES;
        KThread* best = nullptr;
        for (auto& t : m_threads) {
            if (t->state != KThread::State::Ready || t->core != core) continue;
            if (!best || t->priority < best->priority ||
                (t->priority == best->priority && t->last_run < best->last_run))
                best = t.get();
        }
        if (best) {
            m_currentCore = core;
            return best;
        }
    }
    return nullptr;
}

void Kernel::SwitchTo(const std::shared_ptr<KThread>& t) {
    if (t != m_current) {
        // Los flags de coma flotante que dejo la FPU del PC son del hilo que sale
        Core::FP::FoldHostFlags(m_cpu.GetState().fpsr);
        if (m_current && m_current->state != KThread::State::Terminated)
            m_current->ctx = m_cpu.GetState();      // guardar los registros del que sale
        m_cpu.GetState() = t->ctx;                  // cargar los del que entra
        m_current = t;
        m_cpu.ClearExclusive();
        ++m_stats.context_switches;
    }
    t->last_run = ++m_runCounter;
}

u64 Kernel::Run(u64 budget) {
    if (CoresRunning()) return 0;   // los nucleos ya corren en sus hilos (StartCores)
    // Sin proceso (programa suelto de la demo o de los tests): CPU directa
    if (m_threads.empty()) return m_cpu.Run(budget);

    u64 done = 0;
    while (done < budget && !m_cpu.IsHalted()) {
        UpdateWaits();
        KThread* next = PickNext();
        if (!next) {
            // Nadie listo pero la GPU (en su hilo) aun trabaja: lo normal es que alguien
            // espere su fence. Esperar en tiempo real (el reloj emulado lo alcanza luego,
            // en el ritmo de main.cpp) y volver, para soltar el candado de la interfaz.
            if (m_gpu.IsBusy() || m_gpu.GetSyncpoints().HasFired()) {
                m_gpu.WaitProgress(std::chrono::milliseconds(2));
                ++m_stats.gpu_waits;
                break;
            }
            // Nadie listo: adelantar el reloj hasta el primer plazo, o es un bloqueo
            u64 earliest = NEVER;
            for (auto& t : m_threads)
                if (t->state == KThread::State::Waiting && t->deadline < earliest) earliest = t->deadline;
            if (earliest == NEVER) {
                HaltWithMessage("Bloqueo: todos los hilos esperan algo que nunca llegara "
                                "(un evento, un mutex...). Mira la lista de hilos.");
                break;
            }
            if (earliest > Now()) {
                m_stats.idle_ticks += earliest - Now();
                m_cpu.AddTicks(earliest - Now());
            }
            continue;
        }
        for (auto& t : m_threads)
            if (t.get() == next) { SwitchTo(t); break; }
        done += m_cpu.Run(std::min(SCHEDULER_SLICE, budget - done));
    }
    return done;
}

// ============================================================================
//  SVC de hilos
// ============================================================================

// svcCreateThread(entry = X1, arg = X2, stack_top = X3, priority = W4, core = W5) -> W1 = handle
void Kernel::SvcCreateThread(CPUState& s) {
    const u64 entry = s.x[1], arg = s.x[2], stack_top = s.x[3];
    const s32 priority = static_cast<s32>(s.x[4]);
    s32 core = static_cast<s32>(s.x[5]);
    if (priority < 0 || priority > 63) { SetResult(s, Result::InvalidPriority); return; }
    if (core == -2) core = 0;                       // "el nucleo por defecto del proceso"
    if (core < 0 || core >= NUM_CORES) { SetResult(s, Result::InvalidCoreId); return; }
    const u64 tls = AllocateTls();
    if (tls == 0) { SetResult(s, Result::OutOfResource); return; }

    auto t = std::make_shared<KThread>();
    t->id = m_nextThreadId++;
    t->priority = priority;
    t->core = core;
    t->affinity = 1ull << core;
    t->tls = tls;
    t->state = KThread::State::Created;
    t->name = "hilo " + std::to_string(t->id);
    t->ctx.Reset();
    t->ctx.pc = entry;
    t->ctx.x[0] = arg;
    t->ctx.sp = stack_top & ~0xFull;
    t->ctx.x[30] = Layout::LOADER_PAGE + 8;         // si la funcion vuelve: svcExitThread
    t->ctx.tpidrro_el0 = tls;
    t->handle = m_handles.Create(t);
    m_threads.push_back(t);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "[HLE] svcCreateThread: %s, entrada 0x%llX, prioridad %d, nucleo %d",
                  t->name.c_str(), static_cast<unsigned long long>(entry), priority, core);
    Logger::Log(Logger::Level::Info, buf);
    SetResult(s, Result::Success);
    s.x[1] = t->handle;
}

// svcStartThread(handle = W0)
void Kernel::SvcStartThread(CPUState& s) {
    auto t = ThreadFromHandle(static_cast<u32>(s.x[0]));
    if (!t) { SetResult(s, Result::InvalidHandle); return; }
    if (t->state != KThread::State::Created) { SetResult(s, Result::InvalidState); return; }
    t->state = KThread::State::Ready;
    if (m_multicore) m_coreCv.notify_all();
    SetResult(s, Result::Success);
}

// svcExitThread(): el hilo actual termina
void Kernel::SvcExitThread(CPUState&) {
    KThread& t = *Cur();
    t.state = KThread::State::Terminated;
    t.wait = KThread::Wait::None;
    FreeTls(t.tls);
    Cpu().RequestStop();
    Logger::Log(Logger::Level::Info, "[HLE] svcExitThread: " + t.name + " ha terminado");

    const bool any_alive = std::any_of(m_threads.begin(), m_threads.end(),
        [](const auto& x) { return x->state != KThread::State::Terminated; });
    if (!any_alive) {
        m_exited = true;
        Cpu().Halt("Todos los hilos han terminado");
    }
}

// svcGetThreadPriority(handle = W1) -> W1 = prioridad
void Kernel::SvcGetThreadPriority(CPUState& s) {
    auto t = ThreadFromHandle(static_cast<u32>(s.x[1]));
    if (!t) { SetResult(s, Result::InvalidHandle); return; }
    SetResult(s, Result::Success);
    s.x[1] = static_cast<u32>(t->priority);
}

// svcSetThreadPriority(handle = W0, prioridad = W1)
void Kernel::SvcSetThreadPriority(CPUState& s) {
    auto t = ThreadFromHandle(static_cast<u32>(s.x[0]));
    const s32 priority = static_cast<s32>(s.x[1]);
    if (!t) { SetResult(s, Result::InvalidHandle); return; }
    if (priority < 0 || priority > 63) { SetResult(s, Result::InvalidPriority); return; }
    t->priority = priority;
    SetResult(s, Result::Success);
}

// svcGetThreadCoreMask(handle = W2) -> W1 = nucleo, X2 = mascara
void Kernel::SvcGetThreadCoreMask(CPUState& s) {
    auto t = ThreadFromHandle(static_cast<u32>(s.x[2]));
    if (!t) { SetResult(s, Result::InvalidHandle); return; }
    SetResult(s, Result::Success);
    s.x[1] = static_cast<u32>(t->core);
    s.x[2] = t->affinity;
}

// svcSetThreadCoreMask(handle = W0, nucleo = W1, mascara = X2)
void Kernel::SvcSetThreadCoreMask(CPUState& s) {
    auto t = ThreadFromHandle(static_cast<u32>(s.x[0]));
    s32 core = static_cast<s32>(s.x[1]);
    u64 mask = s.x[2];
    if (!t) { SetResult(s, Result::InvalidHandle); return; }
    if (core == -3) core = t->core;                 // "no cambiar el nucleo ideal"
    if (core == -2) core = 0;
    if (core == -1) {                               // "da igual": el actual si esta en la mascara
        if (mask == 0 || (mask >> NUM_CORES) != 0) { SetResult(s, Result::InvalidCombination); return; }
        core = ((mask >> t->core) & 1) ? t->core : std::countr_zero(mask);
    }
    if (core < 0 || core >= NUM_CORES) { SetResult(s, Result::InvalidCoreId); return; }
    if (mask == 0) mask = 1ull << core;
    if (!((mask >> core) & 1) || (mask >> NUM_CORES) != 0) { SetResult(s, Result::InvalidCombination); return; }
    t->core = core;
    t->affinity = mask;
    SetResult(s, Result::Success);
}

// svcGetThreadId(handle = W1) -> X1 = id
void Kernel::SvcGetThreadId(CPUState& s) {
    auto t = ThreadFromHandle(static_cast<u32>(s.x[1]));
    if (!t) { SetResult(s, Result::InvalidHandle); return; }
    SetResult(s, Result::Success);
    s.x[1] = t->id;
}

// ============================================================================
//  Sincronizacion
// ============================================================================

// svcSignalEvent(handle = W0): los hilos que lo esperan se despiertan en UpdateWaits
void Kernel::SvcSignalEvent(CPUState& s) {
    auto ev = m_handles.Get<KEvent>(static_cast<u32>(s.x[0]));
    if (!ev) { SetResult(s, Result::InvalidHandle); return; }
    ev->signaled = true;
    SetResult(s, Result::Success);
}

// svcWaitSynchronization(handles = X1, cantidad = W2, timeout_ns = X3) -> W1 = indice
void Kernel::SvcWaitSynchronization(CPUState& s) {
    const u64 handles_ptr = s.x[1];
    const u32 count = static_cast<u32>(s.x[2]);
    const s64 timeout = static_cast<s64>(s.x[3]);
    if (count > 64) { SetResult(s, Result::OutOfRange); return; }

    std::vector<std::shared_ptr<KObject>> objects;
    for (u32 i = 0; i < count; ++i) {
        const u32 h = m_memory.Read<u32>(handles_ptr + i * 4);
        auto obj = (h == CURRENT_THREAD_PSEUDO_HANDLE) ? std::shared_ptr<KObject>(Cur()) : m_handles.Get(h);
        if (!obj) { SetResult(s, Result::InvalidHandle); return; }
        objects.push_back(std::move(obj));
    }

    if (Cur() && Cur()->cancel_pending) {
        Cur()->cancel_pending = false;
        SetResult(s, Result::Cancelled);
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        if (objects[i]->IsSignaled()) {
            SetResult(s, Result::Success);
            s.x[1] = i;
            return;
        }
    }
    if (timeout == 0) { SetResult(s, Result::TimedOut); return; }
    if (!Cur()) {
        // Programa suelto sin proceso (sin hilos): nadie podra despertarlo nunca
        HaltWithMessage("svcWaitSynchronization: espera sin fin en un programa sin hilos");
        return;
    }

    Cur()->wait_objects = std::move(objects);
    ++m_stats.sync_waits;
    Block(*Cur(), KThread::Wait::Sync, timeout);
}

// svcCancelSynchronization(handle = W0): saca al hilo de su espera (Cancelled)
void Kernel::SvcCancelSynchronization(CPUState& s) {
    auto t = ThreadFromHandle(static_cast<u32>(s.x[0]));
    if (!t) { SetResult(s, Result::InvalidHandle); return; }
    if (t->state == KThread::State::Waiting && t->wait == KThread::Wait::Sync) Wake(*t, Result::Cancelled);
    else t->cancel_pending = true;
    SetResult(s, Result::Success);
}

// svcArbitrateLock(dueno = W0, mutex = X1, tag = W2): espera a que el dueno suelte el mutex
void Kernel::SvcArbitrateLock(CPUState& s) {
    const u32 owner = static_cast<u32>(s.x[0]);
    const u64 addr = s.x[1];
    const u32 tag = static_cast<u32>(s.x[2]);
    if (addr & 3) { SetResult(s, Result::InvalidAddress); return; }
    if (!ThreadFromHandle(owner)) { SetResult(s, Result::InvalidHandle); return; }

    // Si mientras tanto el mutex cambio, el programa lo vuelve a intentar
    if (m_memory.Read<u32>(addr) != (owner | MUTEX_HAS_WAITERS)) { SetResult(s, Result::Success); return; }

    KThread& t = *Cur();
    t.mutex_addr = addr;
    t.mutex_tag = tag;
    t.pending_result = Result::Success;
    ++m_stats.mutex_waits;
    Block(t, KThread::Wait::Mutex, -1);
}

// svcArbitrateUnlock(mutex = X0): el dueno suelta el mutex y hay hilos esperando
void Kernel::SvcArbitrateUnlock(CPUState& s) {
    const u64 addr = s.x[0];
    if (addr & 3) { SetResult(s, Result::InvalidAddress); return; }
    ReleaseMutex(addr);
    SetResult(s, Result::Success);
}

// svcWaitProcessWideKeyAtomic(mutex = X0, key = X1, tag = W2, timeout_ns = X3)
void Kernel::SvcWaitProcessWideKeyAtomic(CPUState& s) {
    const u64 addr = s.x[0], key = s.x[1];
    const u32 tag = static_cast<u32>(s.x[2]);
    const s64 timeout = static_cast<s64>(s.x[3]);
    if (addr & 3) { SetResult(s, Result::InvalidAddress); return; }

    ReleaseMutex(addr);                      // 1) soltar el mutex
    m_memory.Write<u32>(key, 1);             // 2) "hay hilos esperando en esta variable"
    KThread& t = *Cur();
    t.mutex_addr = addr;
    t.mutex_tag = tag;
    t.cv_key = key;
    ++m_stats.condvar_waits;
    Block(t, KThread::Wait::CondVar, timeout);   // 3) esperar senal o plazo
}

// svcSignalProcessWideKey(key = X0, cuantos = W1; <= 0 = todos)
void Kernel::SvcSignalProcessWideKey(CPUState& s) {
    const u64 key = s.x[0];
    const s32 count = static_cast<s32>(s.x[1]);

    std::vector<KThread*> waiters;
    for (auto& t : m_threads)
        if (t->state == KThread::State::Waiting && t->wait == KThread::Wait::CondVar && t->cv_key == key)
            waiters.push_back(t.get());
    std::sort(waiters.begin(), waiters.end(), [](const KThread* a, const KThread* b) {
        return a->priority != b->priority ? a->priority < b->priority : a->wait_order < b->wait_order;
    });
    const size_t n = (count <= 0) ? waiters.size() : std::min<size_t>(size_t(count), waiters.size());
    for (size_t i = 0; i < n; ++i) AcquireMutexAfterWait(*waiters[i], Result::Success);
    if (n == waiters.size()) m_memory.Write<u32>(key, 0);    // ya no queda nadie esperando
}

// ============================================================================
//  Multinucleo: cada nucleo emulado en su propio hilo del PC
// ============================================================================
//
// Cada nucleo repite: coger el candado -> UpdateWaits -> elegir el mejor hilo listo de
// ese nucleo -> cargar sus registros en la CPU del nucleo -> soltar el candado ->
// ejecutar SCHEDULER_SLICE instrucciones (las SVC cogen el candado) -> guardar los
// registros. Sin trabajo, espera (como mucho 1 ms o hasta el primer plazo) a que algo
// lo despierte (m_coreCv). Un hilo solo corre en su nucleo (t->core), como en Horizon.
//
// La memoria es compartida: LDXR/STXR, CAS y LDADD son atomicos de verdad
// (Memory::CompareExchange), y cada CPU se entera de las escrituras en codigo.

u64 Kernel::WallClock(const void* kernel) {
    const auto* k = static_cast<const Kernel*>(kernel);
    const u64 base = k->m_clockBase.load(std::memory_order_acquire);
    if (!k->m_clockRunning.load(std::memory_order_acquire)) return base;
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const s64 elapsed = now - k->m_clockStartNs.load(std::memory_order_acquire);
    return base + (elapsed > 0 ? u64(elapsed) / 32 : 0);   // 31,25 MHz: 1 tick = 32 ns
}

void Kernel::SetMulticore(bool on) {
    StopCores();
    std::lock_guard lock(m_lock);
    if (on == m_multicore) return;
    if (on) {
        m_clockBase = m_cpu.GetTicks();       // el reloj real sigue desde donde iba
        m_clockRunning = false;
        m_cpu.SetClock(&WallClock, this);
    } else {
        const u64 ticks = WallClock(this);
        m_cpu.SetClock(nullptr, nullptr);
        if (ticks > m_cpu.GetTicks()) m_cpu.AddTicks(ticks - m_cpu.GetTicks());
    }
    for (auto& c : m_extraCpus) if (c) c->SetClock(on ? &WallClock : nullptr, on ? this : nullptr);
    m_multicore = on;
}

Core::Interpreter& Kernel::EnsureCoreCpu(s32 core) {
    if (core == 0) return m_cpu;
    auto& slot = m_extraCpus[size_t(core)];
    if (!slot) {
        slot = std::make_unique<Core::Interpreter>(m_memory, u32(core));
        slot->SetSvcHandler([this](u32 imm, CPUState& state) {
            std::lock_guard lock(m_lock);
            HandleSvc(imm, state);
        });
        slot->SetClock(&WallClock, this);
    }
    // Mismas opciones que la CPU principal (la interfaz cambia la de m_cpu)
    if (slot->IsJitEnabled() != m_cpu.IsJitEnabled()) slot->SetJitEnabled(m_cpu.IsJitEnabled());
    if (slot->IsDecodeCacheEnabled() != m_cpu.IsDecodeCacheEnabled()) slot->SetDecodeCacheEnabled(m_cpu.IsDecodeCacheEnabled());
    return *slot;
}

void Kernel::StartCores() {
    std::unique_lock lock(m_lock);
    if (!m_multicore || !m_coreThreads.empty() || m_threads.empty() || m_cpu.IsHalted()) return;
    // Los registros del hilo cargado en la CPU (modo de un hilo) vuelven a su KThread
    if (m_current) {
        if (m_current->state != KThread::State::Terminated) m_current->ctx = m_cpu.GetState();
        m_current.reset();
    }
    for (s32 c = 0; c < NUM_CORES; ++c) EnsureCoreCpu(c).Resume();
    m_coresStop = false;
    m_coresHalted = false;
    m_coreHaltReason.clear();
    m_clockStartNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_clockRunning = true;
    for (s32 c = 0; c < NUM_CORES; ++c) m_coreThreads.emplace_back([this, c] { CoreLoop(c); });
}

void Kernel::StopCores() {
    if (m_coreThreads.empty()) return;
    m_coresStop = true;
    {
        std::lock_guard lock(m_lock);
        for (s32 c = 0; c < NUM_CORES; ++c)
            if (auto* cpu = CoreCpu(c)) cpu->RequestStop();
        m_coreCv.notify_all();
    }
    for (auto& t : m_coreThreads) t.join();
    m_coreThreads.clear();

    std::lock_guard lock(m_lock);
    m_clockBase = WallClock(this);
    m_clockRunning = false;
    // La parada de cualquier nucleo se ve en la CPU principal (la interfaz y los tests miran esa)
    if (m_coresHalted && !m_cpu.IsHalted()) m_cpu.Halt(m_coreHaltReason);
    for (auto& c : m_extraCpus) if (c) c->Resume();
}

KThread* Kernel::PickNextOnCore(s32 core) {
    KThread* best = nullptr;
    for (auto& t : m_threads) {
        if (t->state != KThread::State::Ready || t->core != core || t->on_core >= 0) continue;
        if (!best || t->priority < best->priority ||
            (t->priority == best->priority && t->last_run < best->last_run))
            best = t.get();
    }
    return best;
}

void Kernel::CoreLoop(s32 core) {
    t_core = core;
    std::unique_lock lock(m_lock);
    Core::Interpreter& cpu = *CoreCpu(core);
    const KThread* last = nullptr;
    while (!m_coresStop) {
        UpdateWaits();
        KThread* next = PickNextOnCore(core);
        if (!next) {
            // Nada que hacer en este nucleo: esperar a que despierten a alguien (o al primer plazo)
            auto wait = std::chrono::microseconds(1000);
            u64 earliest = NEVER;
            for (auto& t : m_threads)
                if (t->state == KThread::State::Waiting && t->deadline < earliest) earliest = t->deadline;
            if (earliest != NEVER) {
                const u64 now = Now();
                const u64 us = earliest > now ? (earliest - now) * 32 / 1000 + 1 : 0;
                wait = std::min(wait, std::chrono::microseconds(us));
            }
            if (wait.count() > 0) m_coreCv.wait_for(lock, wait);
            continue;
        }
        std::shared_ptr<KThread> thread;
        for (auto& t : m_threads) if (t.get() == next) { thread = t; break; }
        thread->on_core = core;
        m_coreThread[size_t(core)] = thread;
        cpu.GetState() = thread->ctx;
        cpu.ClearExclusive();
        if (thread.get() != last) ++m_stats.context_switches;
        last = thread.get();
        thread->last_run = ++m_runCounter;

        lock.unlock();
        cpu.Run(SCHEDULER_SLICE);   // sin el candado: aqui es donde los nucleos van a la vez
        lock.lock();

        Core::FP::FoldHostFlags(cpu.GetState().fpsr);
        thread->ctx = cpu.GetState();
        for (unsigned i = 0; i < 2; ++i)
            if (thread->pending_mask & (1u << i)) thread->ctx.x[i] = thread->pending_x[i];
        thread->pending_mask = 0;
        thread->on_core = -1;
        m_coreThread[size_t(core)].reset();

        if (cpu.IsHalted()) {
            // Fin del programa o error en este nucleo: parar todos
            if (!m_coresHalted) {
                m_coreHaltReason = cpu.GetHaltReason();
                m_coresHalted = true;
            }
            m_coresStop = true;
            for (s32 c = 0; c < NUM_CORES; ++c)
                if (auto* other = CoreCpu(c); other && c != core) other->RequestStop();
            m_coreCv.notify_all();
            break;
        }
    }
    t_core = -1;
}

} // namespace NeXo2::HLE
