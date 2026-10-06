# ARM64 CPU interpreter

The interpreter executes guest ARM64 code one instruction at a time
(Fetch -> Decode -> Execute). It is the reference CPU: simple and easy to debug.
The JIT ([jit.md](jit.md)) runs the same code faster, and the tests check that both give
the same results.

## Files

| File | Role |
| :--- | :--- |
| `src/core/arm64/cpu_state.hpp` | Registers: X0-X30, V0-V31 (128-bit), SP, PC, NZCV, TPIDR_EL0, TPIDRRO_EL0, FPCR/FPSR |
| `src/core/arm64/interpreter.hpp/.cpp` | Main loop (`Step`, `Run`, `Halt`), top-level decode, flags, conditions, shifts, extends |
| `interpreter_dp_imm.cpp` | Data processing, immediate |
| `interpreter_dp_reg.cpp` | Data processing, register |
| `interpreter_branch.cpp` | Branches, exceptions, system |
| `interpreter_ldst.cpp` | Loads and stores |
| `interpreter_simd_ldst.cpp` | SIMD/FP loads and stores (`ldr q0`, `stp q0, q1`, `ld1`-`ld4`, `ld1r`...) |
| `interpreter_fp.cpp` | Scalar floating point (`fadd d0`, `fcmp`, `scvtf`, `fmov`...) in half, single and double, and SIMD/FP routing |
| `interpreter_simd.cpp` | All of Advanced SIMD (NEON), vector and scalar, organised by the encoding groups of the manual (`SimdOps`) |
| `interpreter_crypto.cpp` | AES (AESE/AESD/AESMC/AESIMC), SHA1 and SHA256 |
| `fp_ops.hpp/.cpp` | Floating point with the exact ARM rules (NaN, FPCR, FPSR, half precision, estimates, conversions) |
| `simd_common.hpp` | Shared SIMD helpers: lane masks, saturation, a portable 128-bit integer, rounding shifts |
| `src/common/bit_utils.hpp` | `Bits`, `SignExtend`, `RotateRight`, `DecodeBitMasks`, 128-bit multiply high |

The split follows the "Top-level encodings" table of the Arm Architecture
Reference Manual: bits 28..25 of every instruction select the group.

## Implemented (integer, EL0)

| Group | Instructions |
| :--- | :--- |
| Immediate | ADR, ADRP, ADD/ADDS/SUB/SUBS (CMP, CMN, MOV sp), AND/ORR/EOR/ANDS (TST), MOVZ/MOVN/MOVK, SBFM/BFM/UBFM (LSL, LSR, ASR, UBFX, SBFX, BFI, BFXIL, UXTB/H, SXTB/H/W), EXTR (ROR) |
| Register | AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS (MOV, MVN), ADD/SUB shifted and extended, ADC/SBC, CCMP/CCMN, CSEL/CSINC/CSINV/CSNEG (CSET, CINC, CNEG), UDIV/SDIV, LSLV/LSRV/ASRV/RORV, RBIT/REV16/REV32/REV/CLZ/CLS, MADD/MSUB (MUL), SMADDL/UMADDL, SMULH/UMULH |
| Branch / system | B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR, BLR, RET, RETAA/RETAB, SVC, BRK, NOP and all HINTs (PACIASP/AUTIASP/BTI), DMB/DSB/ISB/CLREX, MRS/MSR (NZCV, FPCR, FPSR, TPIDR_EL0, TPIDRRO_EL0, CNTFRQ_EL0, CNTPCT_EL0, CNTVCT_EL0, CTR_EL0, DCZID_EL0), cache maintenance (NOP) and DC ZVA (zeroes 64 bytes) |
| Load / store | LDR/STR (B, H, W, X; unsigned offset, pre/post-index, unscaled, register offset, unprivileged LDTR/STTR), LDRSB/LDRSH/LDRSW, LDR literal, PRFM, LDP/STP/LDPSW/LDNP/STNP, LDXR/STXR/LDAXR/STLXR, LDXP/STXP/LDAXP/STLXP, LDAR/STLR (LDLAR/STLLR), LDAPR, CAS, CASP, LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN, SWP |
| CRC | CRC32B/H/W/X, CRC32CB/H/W/X |
| SIMD/FP loads/stores | LDR/STR b/h/s/d/q (all addressing modes), LDUR/STUR, LDR literal, LDP/STP/LDNP/STNP s/d/q, LD1-LD4/ST1-ST4 (multiple structures, single lane), LD1R-LD4R, post-index by immediate or register |
| Scalar FP | Everything in the "Floating-point" groups, for half (16), single (32) and double (64): FMOV (reg, imm, general<->FP incl. half and V.D[1]), FABS, FNEG, FSQRT, FCVT between all sizes, FRINTN/P/M/Z/A/X/I, FADD, FSUB, FMUL, FDIV, FNMUL, FMAX/FMIN(NM), FMADD/FMSUB/FNMADD/FNMSUB, FCMP/FCMPE, FCCMP/FCCMPE, FCSEL, SCVTF/UCVTF and FCVT{N,P,M,Z,A}{S,U} (integer and fixed point) |
| Vector SIMD | All the Advanced SIMD groups: three same (incl. saturating, halving, rounding, shifts by register, pairwise, polynomial PMUL, all FP incl. FP16, FRECPS/FRSQRTS, FMULX, FABD, FACGE...), three different (long/wide/narrow: SADDL, UMULL, SQDMULL, PMULL incl. 64-bit, ADDHN, SABAL...), two-register misc (incl. SQXTN, SQABS, URECPE, FRECPE, FRSQRTE, FCVTN/FCVTL/FCVTXN, FRINT*), across lanes, copy, permute, EXT, TBL/TBX, modified immediate, shift by immediate (incl. narrowing with rounding/saturation, SRI/SLI, fixed point conversions), by element (incl. FP16, long, SQDMULH, SQRDMLAH, SDOT/UDOT), SQRDMLAH/SQRDMLSH, SDOT/UDOT |
| Scalar SIMD | The scalar forms of all groups above (pairwise, saturating, shifts, by element, FP16...) |
| Crypto | AESE, AESD, AESMC, AESIMC, SHA1C/P/M/H/SU0/SU1, SHA256H/H2/SU0/SU1 |

### Switch 2 specific behaviour

- **PAC** (Pointer Authentication): instructions like `paciasp`, `autiasp`,
  `pacia`, `retaa` are accepted but do not sign pointers. Since signing and
  checking are both no-ops, signed code still works.
- **Timer**: `CNTFRQ_EL0` returns 31.25 MHz (Switch 2). `CNTPCT_EL0` currently
  counts executed instructions (temporary).
- **TLS**: Horizon stores the thread's TLS pointer in `TPIDRRO_EL0`.

### Not implemented

- Features the Switch 2 CPU (Cortex-A78C, ARMv8.2 + some v8.3/v8.4) does not have: SVE, SHA512/SHA3,
  SM3/SM4, BF16, I8MM, FCMA (FCMLA/FCADD), JSCVT, FRINT32/64, MTE.
- Exceptions: an unknown instruction just stops the CPU (`IsHalted()`), with the
  PC left on the instruction that failed.

How complete this is, is measured, not guessed: see [CPU fuzzing](cpu-fuzzing.md).

## Floating point with ARM rules (`fp_ops.hpp/.cpp`)

The PC (x86) and ARM both follow IEEE 754, but they differ in the details, and programs
can see those details:

| Detail | x86 | ARM (what `FP::` does) |
| :--- | :--- | :--- |
| NaN result of an invalid operation (0/0, inf-inf) | negative (`FFC00000`) | positive "default NaN" (`7FC00000`) |
| NaN inputs | depends on the instruction | first signalling NaN (made quiet), then first quiet NaN, keeping its payload |
| FPCR.FZ / FZ16 | - | subnormal inputs and outputs become 0 (marks IDC / UFC) |
| FPCR.DN | - | every NaN result is the default NaN |
| FPCR.AHP | - | "alternative" half precision without infinities or NaN |
| FRECPE / FRSQRTE | different tables | ARM's own estimate algorithm |
| Half precision (16 bits) | no arithmetic | full support |

How it works:

- **Fast path** (`FP::Fast`, inline in the header): with the normal FPCR (round to nearest,
  no FZ, no DN) and no NaN among the inputs, x86 gives exactly the ARM result and flags, so
  `fadd s0, s1, s2` is a single host `addss`. Only a NaN *produced* by the operation is
  replaced with ARM's default NaN. Almost all game code takes this path.
- **Full path**: NaN rules first, then FZ on the inputs, then the operation on the host FPU
  with its rounding mode set like FPCR (`RoundGuard`, `fesetround`), then FZ on the output.
- **Software rounding** (`RoundTo`, the manual's `FPRound`): conversions between sizes,
  float <-> integer/fixed point, FRINT, half precision and fused operations computed in
  double. To avoid rounding twice (wrong in rare cases), the double step truncates and
  keeps a "sticky" bit, and `RoundTo` does the only real rounding.
- **FPSR flags**: flags computed in software go to `fpsr` at once; those of the host FPU stay
  in the FPU and are collected by `FP::FoldHostFlags()` when the program reads FPSR
  (`mrs x0, fpsr`) and on every thread switch (the FPU is shared by all guest threads).
- `fp_ops.cpp` is compiled with `-frounding-math` (GCC/Clang) or `/fp:strict` (MSVC), so the
  compiler does not move operations around the rounding-mode changes.

## Tests

`tests/cpu_tests.cpp` builds the `nexo2_tests` executable. Each test loads a
program into memory, runs it until `brk #0` and checks registers and memory.

- `tests/programs/*.S`: small ARM64 programs written by hand.
- `tests/programs/c_functions.c`: normal C code compiled by clang for ARM64.
  Results are compared with the same function run on the PC.
- `tests/generated/test_programs.hpp`: machine code of all the above, made by
  `tools/asm2cpp.py`. It is committed, so you don't need LLVM to build.

## Differential tests (SIMD/FP)

`tests/programs/simd/*.S` are run on a reference ARM64 (QEMU) by `tools/gen_simd_tests.py`, which
stores **every** register at the end (x0-x28, NZCV, v0-v31) in `tests/generated/simd_tests.hpp`.
`tests/simd_tests.cpp` runs the same code in NeXo and compares register by register, so the
expected values come from real ARM behaviour (NaN, saturation, rounding...) and not from us.

## CPU fuzzing (against a real ARM)

`tests/cpu_fuzz_tests.cpp` runs ~43 000 random instructions of every encoding group and
compares each result with QEMU. See [CPU fuzzing](cpu-fuzzing.md).

## How to add a new instruction

1. Find its encoding in the Arm ARM (or run `llvm-mc -triple=aarch64 -show-encoding`).
2. Add it to the matching `interpreter_*.cpp`, next to similar instructions.
   Return `true` when handled, `false` if the encoding is not supported.
3. Write a small program in `tests/programs/` that uses it and ends in `brk #0`.
4. Run `python tools/asm2cpp.py` (needs LLVM) to regenerate the header.
5. Add a `TEST(...)` in `tests/cpu_tests.cpp` checking the result, and run the tests.

## Performance

Measured with NX-FixCheat (libnx console homebrew), g++ -O2, one x86-64 core:

| Version | Guest instructions / s |
| :--- | :--- |
| First working version | ~42 M |
| Two-level page table + code page cache + inlined helpers | ~95 M |
| + decode cache (fast handlers for hot instructions) | ~170 M |

Floating point (micro-benchmark, ns per guest instruction, loop overhead included; an integer
ADD costs 4.4): scalar FADD 5.6, FMADD 5.6, vector FADD .4S 11, vector FMLA .4S 17.

What made the difference (profiled with valgrind/callgrind):

1. **Memory lookups.** Every access used to search a `std::unordered_map` of pages (~30 % of
   the time). `Memory` now has a two-level page table like a real MMU: level 1 has one slot
   per 2 MB, level 2 has 512 pointers to 4 KB pages (created on demand). `Read<T>`/`Write<T>`
   have a fast path for values that do not cross a page border.
2. **Instruction fetch.** `Interpreter::Run` keeps a pointer to the current code page; while
   the PC stays inside it, fetching is a plain array read. The pointer is requested again on
   every `Run()` call, because `Memory::Clear()` (loading another program) only happens
   between calls.
3. **Inlining.** `X`, `SetX`, `AddWithCarry`, `ShiftReg`, `ConditionHolds`, `Execute`... are
   defined `inline` in `interpreter.hpp`, so the compiler puts them inside each instruction
   handler instead of calling a function per register read.
4. **Small handlers.** `ExecDataProcReg` only dispatches; each family (logical, add/sub,
   CSEL, 2-source, 3-source...) has its own small function.

Release builds also enable link-time optimisation (`INTERPROCEDURAL_OPTIMIZATION_RELEASE`
in `CMakeLists.txt`). The "Pantalla" window shows the live speed (frames/s and M instr/s).

Measured in host instructions per emulated instruction (callgrind, exact): 243 at the
start, 147 after the first round, ~60 with the decode cache.

## Decode cache (`interpreter_fast.cpp`)

Decoding means looking at the bits of an instruction to know what it is and where its
operands are. Without a cache that happens every time an instruction runs, even inside a
loop that runs it millions of times. The decode cache does it **once per address**:

```
Run() with cache (RunCached):
  page of the PC -> CachePage (1024 entries, one per instruction of the 4 KB page)
  entry.fn(entry)            first time: DecodeAndRun -> Decode() fills the entry
                             next times: the fast handler, operands already extracted
```

- `DecodedInstr` keeps the handler pointer and pre-extracted operands: registers,
  immediates, logical masks (`DecodeBitMasks` runs once), branch targets (absolute).
- **Fast handlers** only for the hot instructions: ADD/SUB (imm and register), logical
  (imm and register), MOV, MOVZ/MOVK, LSL/LSR/ASR (imm and register), bitfield, CSEL family,
  MADD, B/BL/B.cond/CBZ/TBZ/BR/BLR/RET, LDR/STR (imm12, imm9, pre/post, register), LDP/STP,
  and floating point: FADD/FSUB/FMUL/FDIV/FNMUL, FMADD family, FMOV/FABS/FNEG (S and D),
  vector FADD/FSUB/FMUL/FDIV/FMLA/FMLS (.2S, .4S, .2D).
  Many are templates (`AddSubReg<SUB, FLAGS, SHIFTED>`, `LoadStoreFixed<MODE, STORE, BYTES>`),
  so the common case has no runtime checks at all.
- Everything else uses `Generic`, which calls the normal decoder (`Execute`). Rule:
  `Decode()` only picks a fast handler when the normal code would accept that encoding,
  so invalid encodings produce exactly the same error.
- **Self-modifying code**: `Memory` marks pages the CPU has decoded (`MarkCode`). The first
  write to such a page is recorded and sets the CPU's `m_attention` flag; the CPU then drops
  the cache of that page before the next instruction. Loading another program
  (`Memory::Clear`) changes `Generation()` and drops everything.
- **XZR trick**: `CPUState::x` has 32 entries; `x[31]` is always 0, so reading register 31
  as XZR needs no check.
- One check per instruction in the loop: `m_attention` is set by `Halt()` and by code writes.

**Verification** (`tests/decode_cache_tests.cpp`): two CPUs, one with and one without the
cache, run the same random instruction (36 000 of them, every fast family plus whole random
groups) from the same random state, and must end with identical registers, flags, PC,
memory and halt status. Plus a self-modifying program and a full `libnx_init.nro` run.
Deliberately breaking one handler makes the test fail with the exact instruction.

The cache can be switched off in the Diagnostics window (or with
`Interpreter::SetDecodeCacheEnabled(false)`) to compare speeds.

The next big step is the JIT: translating whole blocks of ARM64 code to x86-64. A typical libnx
console homebrew needs ~15-20 M instructions per frame, so full speed (60 frames/s)
needs over 1 000 M instructions/s: only a JIT gets there.
