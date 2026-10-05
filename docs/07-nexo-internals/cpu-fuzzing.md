# CPU fuzzing (comparing against a real ARM)

How do we know the CPU is complete and correct? We don't guess: we run **tens of thousands
of random instructions** on NeXo and on a reference ARM64 (QEMU) and compare the results.

## The idea

```
tools/cpu_fuzz.py (Linux/WSL, once)               tests/cpu_fuzz_tests.cpp (every test run)
-----------------------------------               ----------------------------------------
for each encoding group ("family"):               for each record of cpu_fuzz.bin:
  random instruction (fixed bits + random bits)     same initial state (from the seed)
  initial state from a seed                         run the instruction in NeXo
  run it in QEMU (-cpu cortex-a76)                  hash of the final state
  save: instr, seed, result, hash, FPSR    ---->    compare with QEMU's hash and FPSR
        -> tests/generated/cpu_fuzz.bin
```

- **Families** = the encoding groups of the Arm ARM ("Top-level encodings" and the tables
  of each group): integer immediate/register/memory, CRC32, every scalar FP group, every
  Advanced SIMD group (vector and scalar), crypto, SIMD loads/stores. Each one is a
  32-character pattern in `tools/cpu_fuzz.py` (`0`/`1` = fixed bit, `x` = random).
- **Initial state** (`tests/cpu_fuzz/cpu_fuzz_state.h`, shared by both sides): general
  registers mix small numbers, special values (0, -1, INT_MIN...) and pointers into a 4 KB
  data area; vector registers mix random bits with "interesting" floats (zeros,
  infinities, NaN, subnormals...); FPCR is 0 half of the time and otherwise has a random
  rounding mode, FZ, DN, AHP and FZ16.
- **Hash**: FNV-1a of x0-x30, SP, NZCV, v0-v31 and the 4 KB data area. FPSR is compared
  separately.
- Both NeXo paths are checked: **with** the decode cache (fast handlers) and **without** it.

## How QEMU runs one instruction

`tests/cpu_fuzz/cpu_fuzz_harness.c` is a small AArch64 Linux program run with
`qemu-aarch64`. For each record:

1. `raise(SIGUSR1)`. The signal handler replaces the saved registers (in the `ucontext`)
   with the initial state and sets the PC to a code page holding `[instruction][udf]`.
2. When the handler returns, the CPU runs the instruction, then hits `udf` -> `SIGILL`.
3. The `SIGILL` handler copies the final state and restores the saved context, so
   execution continues after `raise()`.

If the `SIGILL` arrives on the instruction itself, it is **not valid**; a `SIGSEGV`/`SIGBUS`
means it touched memory outside the data area (not compared).

## Reading the result

```
familia                                  correcta  INCORR.  falta  cobertura acepta-inv   FPSR
SIMD: tres iguales                            708        0      0     100.0%         0      0
...
TOTAL: 15629 de 15629 instrucciones validas correctas (100.0%)
```

| Column | Meaning | Test fails? |
| :--- | :--- | :--- |
| correcta | same result as ARM | - |
| INCORR. | NeXo runs it but the result is different | yes |
| falta | ARM runs it, NeXo does not implement it (coverage) | no |
| acepta-inv | ARM says "not valid", NeXo runs it anyway | yes |
| FPSR | same result, different floating point flags | yes |

## Where we are

| | Valid instructions correct |
| :--- | :--- |
| Before (13 coarse families) | 54 % |
| After the SIMD/FP rewrite | **100 %** of ~15 600 valid instructions (43 families x 1000), 0 wrong flags, 0 invalid accepted |

Also checked with 5000 per family and other seeds (~78 000 valid instructions each), and
under AddressSanitizer + UndefinedBehaviorSanitizer.

Bugs it found on the way (and that hand-written tests had missed): NaN sign in FNMADD,
saturating shifts by large amounts, RBIT on `.16b` written as `.8h`, FCVTXN not decoded,
FRECPS rounding twice, FRSQRTS overflowing in an intermediate step, IDC not set when an
input was a NaN, LDXP/STXP/CASP missing, invalid encodings accepted (EXTR with N != sf,
CSEL with op2 = 1x, LDAR with Rs != 11111, LDPSW non-temporal...).

## Known QEMU differences

The tool handles three kinds of QEMU problems automatically (see `tools/cpu_fuzz.py`):

- **QEMU aborts** on some invalid FP16 encodings ("code should not be reached"): those
  records are found by splitting the list in halves and removed.
- **QEMU too lax** (`qemu_too_lax`): encodings the manual marks as invalid but QEMU runs
  (FP16 across-lanes/pairwise with bit 22 = 1, MOVI with op = 1 and o2 = 1, scalar FP16
  FABS/FNEG). They are stored as "not valid", like a real CPU.
- **Untestable** (`qemu_untestable`): PAC instructions (the Switch 2 CPU has them, QEMU's
  Cortex-A76 does not) and scalar half-precision FCVTZS/FCVTZU to fixed point (QEMU writes 32
  bits instead of 16). Skipped.

## Usage

Running the test needs nothing special: `cpu_fuzz.bin` is committed.

```cmd
build\Release\nexo2_tests.exe
```

Debugging one failure (the test prints its index):

```bash
NEXO2_FUZZ_SHOW=1234 ./nexo2_tests          # NeXo's final state for record 1234
python3 tools/cpu_fuzz.py --show 1234        # QEMU's final state for the same record
NEXO2_FUZZ_VERBOSE=1 ./nexo2_tests           # also list missing / accepted-invalid ones
```

Regenerating (Linux/WSL: `apt install qemu-user gcc-aarch64-linux-gnu`):

```bash
python3 tools/cpu_fuzz.py                                      # 1000 per family -> tests/generated/cpu_fuzz.bin
python3 tools/cpu_fuzz.py --count 5000 --seed 7 --output /tmp/big.bin
NEXO2_FUZZ_FILE=/tmp/big.bin ./nexo2_tests                      # run a bigger set
```

## Not covered by the fuzzer

Branches and system instructions (they change the PC or the environment) are covered by the
hand-written programs in `tests/programs/` and by the homebrew tests instead.
