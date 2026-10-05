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
  `SyscallAvailableHint`, `HosVersion` (20.1.0), `EndOfList` (pointer to "NeXo 2 HLE loader").

## Implemented SVCs

| SVC | Name | Notes |
| :--- | :--- | :--- |
| 0x01 | SetHeapSize | Size must be a multiple of 2 MB. Heap at `0x80000000`. |
| 0x06 | QueryMemory | Fills `MemoryInfo` from the region map (free gaps included). |
| 0x07 | ExitProcess | Halts the CPU. |
| 0x0B | SleepThread | No-op (single thread). |
| 0x16 | CloseHandle | Removes the handle from the handle table. |
| 0x1E | GetSystemTick | Same counter as `CNTPCT_EL0` (instructions executed). |
| 0x26 | Break | Halts the CPU (program aborted). |
| 0x27 | OutputDebugString | Text shown in the "Programa" window and the console. |
| 0x1F | ConnectToNamedPort | Only "sm:". See [ipc-and-services.md](ipc-and-services.md). |
| 0x21 | SendSyncRequest | IPC message in TLS. |
| 0x22 | SendSyncRequestWithUserBuffer | IPC message in a user buffer. |
| 0x29 | GetInfo | Region addresses/sizes, memory totals, core mask, entropy, program id. |

Any other SVC halts the CPU with its name, e.g. `SVC 0x1F (ConnectToNamedPort) no implementada`.
That message tells you exactly what to implement next.

## Test homebrew

`tests/programs/nro_hello/` is a small homebrew without libnx: its own `crt0.S`,
a linker script and `main.c` that calls SVCs with inline assembly.
`tools/make_nro.py` builds it into `tests/generated/hello.nro` (with an `ASET`
block holding a NACP: title "NeXo Hello"). `tests/loader_tests.cpp` runs it and
checks every line it prints.

## Next steps

1. ~~IPC + service manager (`sm:`)~~ done: see [ipc-and-services.md](ipc-and-services.md). Next: `appletOE`, `hid`, `time`, `fsp-srv`.
2. Threads and synchronization SVCs (`CreateThread`, `WaitSynchronization`, `ArbitrateLock`...).
3. Permission checks on memory access (today the region map is informational).
4. SIMD/FP in the CPU (needed by almost every real program).
