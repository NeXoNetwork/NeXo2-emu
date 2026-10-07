# GPU shaders (Maxwell SM 5.x interpreter)

Phase 2 of the GPU runs the programs that the game or homebrew uploads for the GPU:
vertex shaders and pixel (fragment) shaders, in the native **Maxwell** instruction set
(SM 5.3 on the Switch's GM20B). NeXo decodes the binary and runs it in a software
interpreter, one thread (one vertex or one pixel) at a time. A Vulkan backend can reuse the
same decoder later (phase 3).

## Files

| File | Role |
| :--- | :--- |
| `src/video_core/shader.hpp` | `ShaderInstr`, `ShaderHeader` (SPH), `ShaderEnv` (what a shader reads from outside), `ShaderProgram`, `ShaderInterpreter` |
| `src/video_core/shader_decode.cpp` | 64-bit instruction -> `ShaderInstr` (opcode table) |
| `src/video_core/shader_exec.cpp` | The interpreter |
| `tests/gpu_shader_tests.cpp` | Decoder checks + the random-program test |
| `tools/shader_fuzz.py` | Generates `tests/generated/shader_fuzz.bin` |
| `tests/shaders/`, `tools/gen_shader_bins.py` | GLSL test shaders compiled to `tests/generated/shader_bins.hpp` |

## Program layout

A shader in GPU memory starts with the **SPH** (Shader Program Header, 0x50 bytes,
documented in NVIDIA's open-gpu-doc). We use: the shader type, `KillsPixels`, `MrtEnable`,
and for pixel shaders `OmapTarget` (which RGBA components each render target receives) and
`OmapDepth`.

The code follows at +0x50, in groups of 32 bytes: one **scheduling word** (stall counts,
barriers; only matters for timing on real hardware, so the interpreter skips it) and three
64-bit instructions. `ShaderProgram` decodes lazily, a group the first time it is reached.

## Instruction encoding

Opcodes have variable length in the top bits. The decoder is a table of
`(mask, match)` rows on the high word. Most ALU instructions come in families that differ
only in where operand B comes from:

| Form | High bits (FADD example) | Operand B | Operand C (3-source ops) |
| :--- | :--- | :--- | :--- |
| register | `0x5C58` | `R[20..27]` | `R[39..46]` |
| constant buffer | `0x4C58` | `c[34..38][20..33 * 4]` | `R[39..46]` |
| immediate | `0x3858` | 19 bits + sign in bit 56 (floats: the top 20 bits of the float) | `R[39..46]` |
| constbuf in C | `0x51xx/0x52xx/0x53xx` | `R[39..46]` | `c[...]` |
| 32-bit immediate | `FADD32I 0x08..`, `MOV32I 0x010` | bits 20..51 | — |

Common fields: `Rd` bits 0..7, `Ra` 8..15, guard predicate 16..18 (7 = PT, always true),
negate it with bit 19. **SSY, PBK, PCNT and CAL are not predicated**: those bits are part
of their target offset (getting this wrong skips every loop setup).

Sources used for the encodings (both MIT licensed): the envytools disassembler (`envydis`,
`gm107.c`) and mesa's nouveau code emitter for GM107. They document the hardware; NeXo's code is
written from scratch.

## Supported instructions

| Group | Instructions |
| :--- | :--- |
| Flow | EXIT, BRA, JMP, SSY/SYNC, PBK/BRK, PCNT/CONT, CAL/RET, KIL, NOP (and DEPBAR, BAR, MEMBAR as no-ops) |
| Move/convert | MOV, MOV32I, S2R, SEL, F2F (incl. f16, rounding to integer), F2I, I2F (byte/half select), I2I |
| Float | FADD(32I), FMUL(32I) with post-scale and the "0 * x = 0" mode, FFMA(32I) (fused), MUFU (sin, cos, ex2, lg2, rcp, rsq, sqrt), RRO, FMNMX, FSET, FSETP, FCMP |
| Integer | LOP(32I), IADD(32I) with carry and `.PO`, IMUL(32I), IMAD (incl. high half), ISCADD, XMAD (all modes), IMNMX, ISET, ISETP, ICMP, SHL, SHR, POPC, BFI, BFE, FLO |
| Memory | ALD/AST (attributes), IPA (interpolation), LDC (indexed constant buffer), LDL/STL (local memory) |
| Textures | TEX, TEXS, TLDS, TXD, TXQ (see [gpu-textures.md](gpu-textures.md)) |
| Not yet | TLD, TLD4 (`textureGather`), image loads/stores, 64-bit floats, LOP3, PSETP and others that uam does not emit |

Notes:

- **Divergence.** Real hardware runs 32 threads together, and SSY/SYNC, PBK/BRK, PCNT/CONT exist
  to re-join threads that took different paths. With one thread a stack is enough: SSY pushes
  a target, SYNC pops up to the last SSY entry and jumps there (BRK and CONT do the same
  for PBK/PCNT).
- **RRO + MUFU.** On hardware, RRO puts the argument into a special range-reduced format
  that only MUFU understands. NeXo's MUFU takes plain floats, so RRO only applies abs/neg.
- **XMAD** multiplies 16x16 bits; three of them make a 32-bit multiply.
  Modes: `psl` shifts the product left 16, `mrg` puts B's low half in the result's high half,
  `cbcc` adds `B << 16`, `clo/chi` take C's low/high half.
- **IADD/ISCADD with both negate bits** is `.PO` (a + b + 1): an adder with a single
  carry-in can't negate both inputs.

## Shader environment

`ShaderEnv` is how a shader sees the rest of the GPU:

| Call | Meaning |
| :--- | :--- |
| `ReadConst(index, offset)` | `c[index][offset]`: constant buffers bound to the stage |
| `ReadAttribute` / `WriteAttribute` | the `a[]` attribute space: vertex inputs, outputs, system values |
| `Interpolate(addr, mode)` | IPA in pixel shaders: 0 linear, 1 perspective (the shader multiplies by w), 2 flat |
| `SystemRegister` | S2R |
| `SampleTexture(handle, request)` / `QueryTexture` | texture instructions ([gpu-textures.md](gpu-textures.md)) |
| `TextureConstbuf()` | the `c[]` slot that holds texture handles (`SetBindlessTexture`) |

Attribute addresses: `a[0x70..0x7C]` position, `a[0x80 + 16*n]` generic attribute *n*,
`a[0x2F8]` InstanceID, `a[0x2FC]` VertexID, `a[0x3FC]` front facing (pixel shaders). In a pixel
shader `a[0x7C]` interpolates 1/w; the shader computes `w = rcp(1/w)` and multiplies the
perspective attributes by it.

**deko3d constant buffers:** `c[0]` driver data, `c[1]` compiler constants (from the `.dksh`),
uniform buffer binding *N* = `c[2 + N]`.

Pixel shader outputs are registers: the enabled components of each render target, packed
in order from R0; depth (if written) goes in the register after the colours plus one.

## Tests

`Shader_Decode` checks fields of instructions taken from real uam output.

`Shader_Fuzz` reads `tests/generated/shader_fuzz.bin`. `tools/shader_fuzz.py` does this:

1. Generates a random GLSL vertex shader with 16 outputs (8 float, 8 int): arithmetic,
   divisions by constants, bit operations, comparisons and selects, conversions,
   transcendental functions, and if/else, loops and loops with `break`.
2. Compiles it with **uam** (deko3d's shader compiler) into real Maxwell code.
3. Computes the expected result in Python (numpy float32 and exact integers).

The interpreter must match: integers exactly, floats within a tolerance scaled to the size
of the intermediate values. One run uses 300 programs (4800 outputs); four different seeds
(1200 programs) all pass.

Two compiler issues are kept out of the generator on purpose:

- With a non-constant divisor, uam emulates integer division with floats, and the result is
  inexact (uam prints a warning). The fuzzer only divides by constants.
- The mesa version inside uam turns `x * -(2^n ± 1)` into the wrong shift-add (for example
  `x * -7` becomes `x * -9`). The fuzzer never multiplies by negative constants.

To isolate a bug, `--op i:xor` (or `f:div`, etc.) generates programs that use a single operation.

### Rebuilding the test data (optional)

```sh
git clone https://github.com/devkitPro/uam && cd uam && meson setup build && ninja -C build
python3 tools/shader_fuzz.py --uam uam/build/uam          # tests/generated/shader_fuzz.bin
python3 tools/gen_shader_bins.py --uam uam/build/uam      # tests/generated/shader_bins.hpp
python3 tools/make_nro.py                                 # tests/generated/triangle.nro
```
