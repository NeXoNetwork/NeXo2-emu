# ARM64 CPU interpreter

The interpreter executes guest ARM64 code one instruction at a time
(Fetch -> Decode -> Execute). It is the reference CPU: simple and easy to debug.
The Ballistic JIT will later run the same code faster, and the tests here will
check that both give the same results.

## Files

| File | Role |
| :--- | :--- |
| `src/core/arm64/cpu_state.hpp` | Registers: X0-X30, SP, PC, NZCV, TPIDR_EL0, TPIDRRO_EL0, FPCR/FPSR |
| `src/core/arm64/interpreter.hpp/.cpp` | Main loop (`Step`, `Run`, `Halt`), top-level decode, flags, conditions, shifts, extends |
| `interpreter_dp_imm.cpp` | Data processing, immediate |
| `interpreter_dp_reg.cpp` | Data processing, register |
| `interpreter_branch.cpp` | Branches, exceptions, system |
| `interpreter_ldst.cpp` | Loads and stores |
| `src/common/bit_utils.hpp` | `Bits`, `SignExtend`, `RotateRight`, `DecodeBitMasks`, 128-bit multiply high |

The split follows the "Top-level encodings" table of the Arm Architecture
Reference Manual: bits 28..25 of every instruction select the group.

## Implemented (integer, EL0)

| Group | Instructions |
| :--- | :--- |
| Immediate | ADR, ADRP, ADD/ADDS/SUB/SUBS (CMP, CMN, MOV sp), AND/ORR/EOR/ANDS (TST), MOVZ/MOVN/MOVK, SBFM/BFM/UBFM (LSL, LSR, ASR, UBFX, SBFX, BFI, BFXIL, UXTB/H, SXTB/H/W), EXTR (ROR) |
| Register | AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS (MOV, MVN), ADD/SUB shifted and extended, ADC/SBC, CCMP/CCMN, CSEL/CSINC/CSINV/CSNEG (CSET, CINC, CNEG), UDIV/SDIV, LSLV/LSRV/ASRV/RORV, RBIT/REV16/REV32/REV/CLZ/CLS, MADD/MSUB (MUL), SMADDL/UMADDL, SMULH/UMULH |
| Branch / system | B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR, BLR, RET, RETAA/RETAB, SVC, BRK, NOP and all HINTs (PACIASP/AUTIASP/BTI), DMB/DSB/ISB/CLREX, MRS/MSR (NZCV, FPCR, FPSR, TPIDR_EL0, TPIDRRO_EL0, CNTFRQ_EL0, CNTPCT_EL0, CNTVCT_EL0) |
| Load / store | LDR/STR (B, H, W, X; unsigned offset, pre/post-index, unscaled, register offset), LDRSB/LDRSH/LDRSW, LDR literal, PRFM, LDP/STP/LDPSW/LDNP/STNP, LDXR/STXR/LDAXR/STLXR, LDAR/STLR, CAS, LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN, SWP |

### Switch 2 specific behaviour

- **PAC** (Pointer Authentication): instructions like `paciasp`, `autiasp`,
  `pacia`, `retaa` are accepted but do not sign pointers. Since signing and
  checking are both no-ops, signed code still works.
- **Timer**: `CNTFRQ_EL0` returns 31.25 MHz (Switch 2). `CNTPCT_EL0` currently
  counts executed instructions (temporary).
- **TLS**: Horizon stores the thread's TLS pointer in `TPIDRRO_EL0`.

### Not implemented yet

- SIMD / floating point (`q0`, `d0`, `fadd`, `ld1`...). This is the biggest gap:
  real games and the SDK use it everywhere, even `memcpy`.
- LDXP/STXP, CASP, LDAPR, CRC32, other system registers.
- Exceptions: an unknown instruction just stops the CPU (`IsHalted()`), with the
  PC left on the instruction that failed.

## Tests

`tests/cpu_tests.cpp` builds the `nexo2_tests` executable. Each test loads a
program into memory, runs it until `brk #0` and checks registers and memory.

- `tests/programs/*.S`: small ARM64 programs written by hand.
- `tests/programs/c_functions.c`: normal C code compiled by clang for ARM64.
  Results are compared with the same function run on the PC.
- `tests/generated/test_programs.hpp`: machine code of all the above, made by
  `tools/asm2cpp.py`. It is committed, so you don't need LLVM to build.

## How to add a new instruction

1. Find its encoding in the Arm ARM (or run `llvm-mc -triple=aarch64 -show-encoding`).
2. Add it to the matching `interpreter_*.cpp`, next to similar instructions.
   Return `true` when handled, `false` if the encoding is not supported.
3. Write a small program in `tests/programs/` that uses it and ends in `brk #0`.
4. Run `python tools/asm2cpp.py` (needs LLVM) to regenerate the header.
5. Add a `TEST(...)` in `tests/cpu_tests.cpp` checking the result, and run the tests.
