# NRO loader and HLE kernel

How NeXo 2 starts a homebrew `.nro` and answers its system calls.

## Flow

```
System::LoadNroFile("hello.nro")
  -> Memory::Clear(), Interpreter::Reset(), Kernel::Reset()
  -> Loader::LoadNro()             copy image to CODE_BASE, zero .bss, mark regions
  -> Kernel::SetupHomebrewProcess() stack, TLS, loader config, entry registers
Interpreter::Run()
  -> "svc #n"  -> Kernel::HandleSvc(n)
  -> svcExitProcess -> CPU halts, Kernel::HasExited() == true
```

| File | Role |
| :--- | :--- |
| `src/core/loader/nro.hpp/.cpp` | Validates and maps an NRO (format: [../04-formats/nro.md](../04-formats/nro.md)) |
| `src/core/hle/kernel.hpp/.cpp` | Process setup + SVC dispatcher |
| `src/core/memory/memory.hpp` | Pages + region map (`MapRegion`, `QueryRegion`) |
| `src/core/system.hpp/.cpp` | Ties it all together; used by `main.cpp` and the tests |

## Process memory layout (fixed, no ASLR yet)

| Address | Size | What |
| :--- | :--- | :--- |
| `0x0008000000` | image | NRO: `.text` (R-X), `.rodata` (R--), `.data`+`.bss` (RW-) |
| `0x0080000000` | up to 1 GB | Heap (`svcSetHeapSize`) |
| `0x00C0000000` | 1 GB | Alias region (reserved) |
| `0x0100000000` | 1 MB | Main thread stack (SP starts at the top) |
| `0x01FFFE0000` | 4 KB | HLE loader page: exit stub, argv, config entries |
| `0x01FFFF0000` | 4 KB | TLS of the main thread (`TPIDRRO_EL0`) |

All of it fits in the 12 GB that `Core::Memory` supports. The values are in
`HLE::Layout` (kernel.hpp) and are reported to programs through `svcGetInfo`.

## Homebrew ABI (entry)

Following [switchbrew Homebrew_ABI](https://switchbrew.org/wiki/Homebrew_ABI):

- `X0` = pointer to the loader config list, `X1` = `0xFFFFFFFFFFFFFFFF`.
- `X30` = exit stub (`svc #0x7`): if the program returns from its entry, it exits cleanly.
- Config entries given: `MainThreadHandle`, `AppletType` (Application), `Argv`,
  `SyscallAvailableHint`, `HosVersion` (20.1.0), `NextLoadPath`, `ProcessHandle`
  (the "current process" pseudo-handle, needed by `.so` loaders of ports), `EndOfList`
  (pointer to "NeXo 2 HLE loader").
- `argv[0]` is where the `.nro` is on the SD: if the file is inside the emulated SD folder
  (e.g. `sdmc/switch/game/game.nro`), the program sees `sdmc:/switch/game/game.nro`, so its
  working directory is its own folder and it finds the files next to it. Otherwise
  `sdmc:/switch/<name>.nro`.

## Chain loading (hbmenu)

`NextLoadPath` points to two buffers in the loader page (path 0x200 bytes at +0x600, argv
0x800 bytes at +0x800). hbmenu writes the chosen `.nro` and its argv there with
`envSetNextLoad` and exits. `System::ContinueAfterExit()` (called by the app when a program
ends) then loads that file from the SD folder with that argv. When a program launched this
way exits without asking for anything, it goes back to the menu, like hbloader.

## Implemented SVCs

| SVC | Name | Notes |
| :--- | :--- | :--- |
| 0x01 | SetHeapSize | Size must be a multiple of 2 MB. Heap at `0x80000000`. |
| 0x02 | SetMemoryPermission | Changes the permission in the region map. |
| 0x03 | SetMemoryAttribute | Only checks alignment (no caches to emulate). |
| 0x04 / 0x05 | MapMemory / UnmapMemory | Moves the pages to the new address (`Memory::MovePages`); the source stays inaccessible. libnx uses it for thread stacks. |
| 0x06 | QueryMemory | Fills `MemoryInfo` from the region map (free gaps included). |
| 0x07 | ExitProcess | Halts the CPU. |
| 0x08-0x0F | CreateThread, StartThread, ExitThread, SleepThread, Get/SetThreadPriority, Get/SetThreadCoreMask | Real threads: see [threads.md](threads.md). |
| 0x10 | GetCurrentProcessorNumber | Emulated core of the current thread (0-5). |
| 0x11 | SignalEvent | Wakes threads waiting for it. |
| 0x12 / 0x17 | ClearEvent / ResetSignal | Clears a `KEvent`. |
| 0x13 / 0x14 | MapSharedMemory / UnmapSharedMemory | Copies the content in and remembers the address (hid updates it live). |
| 0x15 | CreateTransferMemory | Returns a handle; the memory stays in place (used by nvdrv). |
| 0x16 | CloseHandle | Removes the handle from the handle table. |
| 0x18 / 0x19 | WaitSynchronization / CancelSynchronization | Blocks the thread (events, threads); a wait nobody can end is reported as a deadlock. |
| 0x1A-0x1D | ArbitrateLock/Unlock, WaitProcessWideKeyAtomic, SignalProcessWideKey | Real mutex and condition variables: see [threads.md](threads.md). |
| 0x1E | GetSystemTick | Same counter as `CNTPCT_EL0` (instructions executed). |
| 0x1F | ConnectToNamedPort | Only "sm:". See [ipc-and-services.md](ipc-and-services.md). |
| 0x21 | SendSyncRequest | IPC message in TLS. |
| 0x22 | SendSyncRequestWithUserBuffer | IPC message in a user buffer. |
| 0x24 / 0x25 | GetProcessId / GetThreadId | Process id fixed; thread ids 1, 2, 3... |
| 0x26 | Break | Halts the CPU (program aborted). |
| 0x27 | OutputDebugString | Text shown in the "Programa" window and the console. |
| 0x29 | GetInfo | Region addresses/sizes, memory totals, core mask, entropy, program id. |
| 0x73 | SetProcessMemoryPermission | Own process only: changes permissions in the region map. |
| 0x77 / 0x78 | MapProcessCodeMemory / UnmapProcessCodeMemory | Own process only: like MapMemory, the destination becomes code. Used by `.so` loaders. |

`NEXO2_SVC_TRACE=1` logs every SVC with its first 4 arguments and its result.

Any other SVC halts the CPU with its name, e.g. `SVC 0x08 (CreateThread) no implementada`.
That message tells you exactly what to implement next.

## Test homebrew

`tests/programs/nro_hello/` is a small homebrew without libnx: its own `crt0.S`,
a linker script and `main.c` that calls SVCs with inline assembly.
`tools/make_nro.py` builds it into `tests/generated/hello.nro` (with an `ASET`
block holding a NACP: title "NeXo Hello"). `tests/loader_tests.cpp` runs it and
checks every line it prints.

## Next steps

1. Speed: decode cache in the interpreter and the dynarmic JIT (done).
2. Permission checks on memory access (today the region map is informational).

Done since this page was first written: IPC and services, SIMD/FP, display, SD card and
controllers (see the other pages in this folder).
