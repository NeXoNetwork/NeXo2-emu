# Threads and scheduler

How NeXo 2 runs programs with several threads (`src/core/hle/kernel_threads.cpp`).

## Two modes

The program sees **6 cores** (CoreMask `0x3F`; the Switch 2 has 8, 2 belong to the system).
They can run in two ways:

| Mode | Who uses it | How |
| :--- | :--- | :--- |
| Single thread (`Kernel::Run`) | tests, `NEXO2_MULTICORE=0` | the 6 cores take turns on the calling host thread; deterministic |
| Multicore (`Kernel::StartCores`) | the app (default) | each emulated core has its own host thread and CPU; they run at the same time |

## Single-thread model (time-sliced cores)

All 6 emulated cores take turns on **one host thread**:

```
Kernel::Run(budget)
  loop:
    UpdateWaits()            expired deadlines, signaled events, finished threads
    PickNext()               next core in rotation -> its best Ready thread
                             (lowest priority value; on a tie, the one that ran least recently)
    SwitchTo(thread)         save registers of the old thread, load the new one
    cpu.Run(SCHEDULER_SLICE) 10 000 instructions, or less if the thread blocks
  nobody ready?
    some thread has a deadline -> advance the clock to it (idle skip)
    nobody can ever wake up    -> stop: "Bloqueo: todos los hilos esperan..."
```

A blocking SVC (wait, sleep, mutex, condition variable) marks the thread `Waiting` and calls
`Interpreter::RequestStop()`: `Run()` returns after that instruction, without halting the CPU,
so the scheduler can switch. The result (`W0`, and `W1` for WaitSynchronization) is written
into the thread's saved registers when it wakes up.

Programs loaded without a process (raw demo / test code) have no threads: `Kernel::Run` just
runs the CPU.

Everything is deterministic: the same program always switches at the same instructions.
`tests/threads_tests.cpp` runs it with and without the decode cache and requires identical
results.

## KThread

| Field | Meaning |
| :--- | :--- |
| `ctx` | Saved registers (when the thread is not on the CPU) |
| `priority`, `core`, `affinity` | 0 = highest priority; ideal core 0-5 |
| `tls` | 0x200-byte TLS (`TPIDRRO_EL0`; IPC messages use its first 0x100 bytes) |
| `state` | Created, Ready, Waiting, Terminated |
| `wait` | Sync (WaitSynchronization), Sleep, Mutex, CondVar |
| `deadline` | In ticks; `~0` = no limit |

TLS slots: the main thread uses the first slot of `TLS_PAGE`; others take slots in pages
mapped downwards from it (8 per page). A finished thread's slot is reused.

When a thread function returns, `LR` points to `svc #0xA` (ExitThread) in the loader page.
When every thread has finished, the process ends.

## Implemented SVCs

| SVC | Name | Notes |
| :--- | :--- | :--- |
| 0x08 | CreateThread | entry X1, arg X2, stack X3, priority W4 (0-63), core W5 (0-5, -2 = default 0) |
| 0x09 | StartThread | Created -> Ready |
| 0x0A | ExitThread | Terminated (signaled for WaitSynchronization) |
| 0x0B | SleepThread | 0/-1/-2 = yield; otherwise sleep with a deadline |
| 0x0C / 0x0D | Get/SetThreadPriority | |
| 0x0E / 0x0F | Get/SetThreadCoreMask | -3 = keep core, -2 = default |
| 0x10 | GetCurrentProcessorNumber | the emulated core running the thread |
| 0x11 | SignalEvent | waiters wake up on the next scheduling step |
| 0x18 | WaitSynchronization | events and threads; timeout 0 = poll; blocks otherwise |
| 0x19 | CancelSynchronization | wakes the thread with 0xEC01, or the next wait returns it |
| 0x1A / 0x1B | ArbitrateLock / ArbitrateUnlock | mutex protocol below |
| 0x1C / 0x1D | WaitProcessWideKeyAtomic / SignalProcessWideKey | condition variables |
| 0x25 | GetThreadId | |

## Mutex and condition variable protocol (as libnx uses it)

The mutex word is `0` (free) or the owner's thread handle, plus bit `0x40000000`
(`MUTEX_HAS_WAITERS`) if someone is waiting.

- **Lock** (user code): compare-and-swap 0 -> own handle. If taken: set the waiters bit and
  call `svcArbitrateLock(owner, addr, own_handle)`. The kernel checks that the word is still
  `owner | waiters` (otherwise the program retries) and puts the thread to sleep.
- **Unlock** (user code): CAS own handle -> 0. If it fails (waiters bit set): `svcArbitrateUnlock`.
  The kernel picks the highest-priority waiter (FIFO on a tie), writes its handle into the word
  (with the waiters bit if more remain) and wakes it: it now owns the mutex.
- **Condition wait**: `svcWaitProcessWideKeyAtomic(mutex, key, handle, timeout)` releases the
  mutex (like unlock), writes 1 to `key` and sleeps. When signaled or timed out, the thread
  first takes the mutex back: if it is free it gets it at once; otherwise it waits for it like
  in ArbitrateLock, and returns the condition result (0 or 0xEA01) when it gets it.
- **Signal**: `svcSignalProcessWideKey(key, count)` wakes `count` waiters (<= 0 = all) by
  priority; `key` goes back to 0 when nobody is left.

## Time

The system clock (`CNTPCT_EL0`, `svcGetSystemTick`) runs at 31.25 MHz: 1 tick = 32 ns,
sleeping 1 ms = 31 250 ticks. In single-thread mode it comes from the instruction counter
(a 998.4 MHz CPU, one instruction per cycle); when every thread sleeps, the scheduler jumps
the clock forward (`Interpreter::AddTicks`) instead of executing nothing, and the app ties it
to real time. In multicore mode every core reads the same real-time clock
(`Interpreter::SetClock` -> `Kernel::WallClock`), which only advances while the cores run.

## Tests

`tests/programs/nro_threads/main.c` (built into `tests/generated/threads.nro`) creates 4
threads on cores 0-3 that add to a shared counter 300 times each under a libnx-style mutex.
The critical section is slow on purpose so the scheduler switches inside it: if the mutex
were broken, additions would be lost. Then it checks the condition variable, joining threads,
sleep, timeouts and priorities. `tests/threads_tests.cpp` also checks that the mutex really
was contended (hundreds of `mutex_waits`), that results are identical without the decode
cache, and that a deadlock is reported.

Breaking `ReleaseMutex` on purpose makes the test fail (the threads deadlock).

## Multicore mode

`Kernel::SetMulticore(true)` + `StartCores()` start one host thread per emulated core. Core 0
uses the `System` CPU; cores 1-5 get their own `Interpreter` (and JIT) the first time they are
needed, with the same JIT/decode-cache settings. Each core loops:

```
lock the kernel (m_lock)
  UpdateWaits()                      same as the single-thread mode
  PickNextOnCore(core)               best Ready thread whose ideal core is this one
  nothing? wait on m_coreCv          at most 1 ms, or until the first deadline
  load its registers into this core's CPU (thread->on_core = core)
unlock
cpu.Run(SCHEDULER_SLICE)             the cores really run in parallel here
lock
  save its registers (+ pending X0/X1, see below), on_core = -1
  CPU halted (exit, svcBreak, unknown opcode)? stop every core
```

A thread only runs on its ideal core (as on Horizon, where a thread created with core -2
stays on the process' default core). Programs that want parallelism create threads on
several cores.

**Kernel lock.** Every SVC takes `m_lock` (a recursive mutex), so the kernel, the services and
IPC run one at a time. Waking a thread notifies `m_coreCv`. If another core wakes a thread
that is still loaded on its core (it blocked a moment ago and is being saved), the result
registers go to `pending_x` and are applied when the core saves the thread (`SetReg`).
`Cpu()`, `Cur()` and `CurCore()` return the CPU, thread and core number of the core running
the current SVC (a `thread_local`), or the single-thread ones.

**Memory.** Shared by all cores:

- Page lookups are lock-free (atomic page-table pointers); creating a page takes a mutex.
- Aligned integer reads/writes are relaxed atomics (the same instruction on x86), so a value
  is never seen half-written.
- LDXR/STXR, LDXP/STXP, CAS/CASP and the LSE atomics (LDADD...) are real atomic operations:
  LDXR remembers the value, STXR does a compare-and-swap with it (`Memory::CompareExchange`).
  The JIT uses one dynarmic `ExclusiveMonitor` shared by all cores (processor id = core).
- Writes to code pages go into a ring of the last 64 pages; every CPU has its own cursor
  (`TakeCodeWrites`) and its own "attention" flag, so each invalidates its own decode cache /
  JIT blocks.
- The kernel's own mutex handoff (`AcquireMutexAfterWait`) uses compare-and-swap too.

**App.** `EmuThread` (main.cpp) starts the cores on Run and stops them on pause, on a halt or
before loading/restarting. The UI takes the kernel lock during its frame (to read threads,
output, regions); to stop the cores from the UI it releases it for a moment
(`StopCoresFromUi`). Step only works in single-thread mode. `NEXO2_MULTICORE=0` turns
multicore off.

**Tests** (`tests/multicore_tests.cpp`): `threads.nro` on 4 host threads (4 rounds, cores 1-3
must have executed code), libnx `__appInit` and IPC, `gpu.nro`, and stopping/resuming
hundreds of times mid-program. Clean with ThreadSanitizer (also running the deko3d examples
with the GPU thread).

## UI

The "Programa" window lists every thread (core, priority, state, what it waits for, PC;
`*` = running on a CPU) and the scheduler counters, and says which mode the cores use.
