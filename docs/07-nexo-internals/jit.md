# JIT (dynarmic)

The interpreter decodes and runs one guest instruction at a time (~180 M instructions/s).
A libnx game needs ~15-20 M instructions per frame, so 60 frames/s needs over 1 000 M/s.
The JIT gets there by **translating** blocks of ARM64 code into x86-64 code once and then
running that code directly on the PC's processor.

NeXo uses **dynarmic** (`externals/dynarmic`, 0BSD licence), a mature ARM -> x86-64/ARM64
recompiler. Our part is the glue: `src/core/arm64/jit_dynarmic.hpp/.cpp`.

## How it fits

The JIT is not a separate CPU. It is another way of doing `Interpreter::Run()`:

```
Interpreter::Run(n)
  JIT off -> decode cache / plain interpreter (as before)
  JIT on  -> JitBackend::Run(n)
               1. copy registers from CPUState into dynarmic
               2. dynarmic translates the blocks it needs and runs them
               3. copy registers back into CPUState
```

Between `Run()` calls the registers live in `Interpreter::GetState()` as always, so the HLE
kernel, the thread scheduler (`SwitchTo` saves/loads `CPUState`), the UI and the tests do
not know whether the JIT is on.

While translated code runs, dynarmic calls us back (`UserCallbacks`):

| Callback | What NeXo does |
| :--- | :--- |
| `MemoryReadCode` | returns the instruction, and marks the page as code (`Memory::MarkCode`) |
| `MemoryRead*/Write*` | `Memory::Read/Write` (only for pages not in the page table, see below) |
| `MemoryWriteExclusive*` | STXR: write only if memory still holds what LDXR read |
| `CallSVC` | the HLE kernel, with the state exactly as the interpreter would give it |
| `InterpreterFallback`, `ExceptionRaised` | instructions dynarmic does not know (PAC, half-precision...) are run by **our interpreter**; `BRK` stops the CPU like before |
| `AddTicks`, `GetTicksRemaining`, `GetCNTPCT` | 1 tick = 1 instruction, same clock as the interpreter |

## Memory: the page table

Calling a function for every load/store would be slow. `Memory::EnableFlatPageTable()`
builds a flat table with one host pointer per 4 KB guest page (2^22 entries = 16 GB of
addresses). Translated code reads it directly: if the entry is not null, the access is a
couple of x86 instructions; if it is null (page not created yet, outside the table, or a
page holding translated code) dynarmic calls `MemoryRead/Write`.

Accesses that cross a page border also use the callbacks (the next host page is unrelated
memory).

## Self-modifying code

A page with translated code gets a **null** entry in the table, so writes to it go through
`Memory::Write`. The first write is noted (`NoteCodeWrite`, the same mechanism as the decode
cache) and the JIT calls `InvalidateCacheRange` for that page; dynarmic drops those blocks
before running them again. Writes from the HLE (services) are caught the same way, checked
at the start of every `Run()` and after every SVC.

## Things to know

- Only for x86-64 PCs for now (`NEXO2_ENABLE_JIT`, ON by default). Without
  `externals/dynarmic` NeXo builds with the interpreter only.
- dynarmic does not compute every FPSR flag exactly and accepts a few encodings that are
  not valid; the **results** are checked against the reference ARM: see below. The
  interpreter remains the exact reference (switch the JIT off in Diagnostics to compare).
- One translated-code cache of 128 MB (virtual) per emulated CPU.
- The FPU flags of the host (`FP::FoldHostFlags`) are not used in JIT mode: dynarmic keeps
  the guest FPSR itself.

## Tests

`nexo2_tests` runs **every test twice**: interpreter and JIT (every CPU the tests create uses
the JIT in the second round). `--interp` / `--jit` run only one.

- CPU fuzzing ([cpu-fuzzing.md](cpu-fuzzing.md)) under the JIT: 100 % of the valid random
  instructions give the same result as the reference ARM (also with 5 000 per family and other
  seeds); FPSR differences and accepted invalid encodings are only reported.
- `DecodeCache_SameResultAsInterpreter` / `RealProgramIdentical` compare JIT against the plain
  interpreter on 36 000 random instructions and a full `libnx_init.nro` run.
- Threads, NRO, IPC, services and self-modifying code tests pass under the JIT.
- Clean under AddressSanitizer + UndefinedBehaviorSanitizer.

## Speed

NX-FixCheat (libnx console homebrew), Release, one x86-64 core:

| | Guest instructions / s |
| :--- | :--- |
| Interpreter + decode cache | ~180 M |
| JIT (steady state) | ~1 100 M (x6) |

The kernel gives each guest thread slices of 10 000 instructions; every slice copies the
registers in and out of dynarmic. That is the next thing to optimise (bigger slices when
only one thread is ready), together with "fastmem" (mapping guest memory into the host
address space so even the page-table lookup disappears).
