# GPU textures (phase 2b)

Shaders read textures through a 32-bit **handle**. The GPU turns the handle into two
32-byte descriptors: one for the image (TIC, "texture image control") and one for the sampler
(TSC, "texture sampler control"). NeXo decodes them, reads the texels from the program's memory
and samples them in software, for vertex and pixel shaders.

Files: `src/video_core/texture.hpp/.cpp`, the texture instructions in `shader_exec.cpp`,
`tests/gpu_texture_tests.cpp`, shaders `tests/shaders/tex*.frag` and `texvert.vert`.

## From the shader to the texels

1. The texture instruction has a 13-bit field `tex.r` (bits 36..48). The handle is the
   word at `c[slot][tex.r * 4]`. The slot comes from `SetBindlessTexture` (method 0x982).
   deko3d uses `c[0]` (its driver constant buffer). In pixel shaders the handles start at
   0x690 (binding *N* is `tex.r = 0x1A4 + N`); in vertex shaders they start at 0x10.
2. Handle bits 0..19 = image index, bits 20..31 = sampler index.
3. The tables (pools) are set with `SetTexHeaderPool` (0x55D address, 0x55F last index) and
   `SetTexSamplerPool` (0x557 address, 0x559 last index). Entry *i* is at `pool + 32 * i`.

A `TextureSampler` is created for each draw. It caches the descriptors and keeps every
mipmap level it has used already decoded (4 values of 32 bits per texel), so memory is read
once per draw, not once per pixel.

## Image descriptor (TIC)

The field positions come from NVIDIA's public header `clb197tex.h` (open-gpu-doc), in its
TEXHEAD_BL and TEXHEAD_PITCH layouts:

| Bits | Field |
| :--- | :--- |
| 0..6 | Format (component sizes or compression) |
| 7..18 | Data type of R, G, B, A (3 bits each: 1 snorm, 2 unorm, 3 sint, 4 uint, 7 float) |
| 19..30 | Swizzle X, Y, Z, W (0 zero, 2 R, 3 G, 4 B, 5 A, 6 integer one, 7 float one) |
| 32..63, 64..79 | Address (low bits are cleared: 512-byte alignment block linear, 32 pitch) |
| 85..87 | Header version: 3 = block linear, 2 = pitch (linear) |
| 99..101 / 102..104 | Block linear: GOBs per block in height / depth (log2) |
| 96..111 | Pitch: bytes per row / 32 |
| 124..127 | Last mipmap level |
| 128..143, 160..175, 176..189 | Width - 1, height - 1, depth or layers - 1 |
| 150 | sRGB |
| 151..154 | Type: 0 1D, 1 2D, 2 3D, 3 cube, 4 1D array, 5 2D array, 7 2D without mipmaps, 8 cube array |
| 191 | Normalized coordinates (0..1) |
| 224..231 | First and last level that the view can use |

## Sampler descriptor (TSC)

| Word | Bits | Field |
| :--- | :--- | :--- |
| 0 | 0..8 | Wrap mode for u, v, w (0 repeat, 1 mirror, 2 clamp to edge, 3 border colour, 4 OpenGL clamp, 5..7 mirror once) |
| 0 | 9, 10..12 | Depth compare enable and function (0 never, 1 less, ... 7 always) |
| 1 | 0..2, 4..5, 6..7 | Magnification, minification and mipmap filter (1 point, 2 linear; mipmap 1 none) |
| 1 | 12..24 | LOD bias (fixed point, 8 fraction bits) |
| 2 | 0..11, 12..23 | Min and max LOD (8 fraction bits) |
| 4..7 | | Border colour (4 floats) |

## Memory layout

**Pitch** textures are rows of `pitch` bytes. **Block linear** textures use the same GOBs as
render targets (64 bytes x 8 rows, see [display.md](display.md)), grouped in blocks of
2^n GOBs in height (and depth for 3D). Mipmap levels follow each other: each level is
padded to whole blocks, and a small level uses a smaller block height (the block height is
halved while half of it still covers the level). Array layers and cube faces are separate
copies of the whole mip chain, each starting at a block boundary.

## Formats

| Kind | Formats |
| :--- | :--- |
| 8/16/32 bits per component | R8, G8R8, A8B8G8R8, X8B8G8R8, R16, R16G16, R16G16B16A16, R32, R32G32, R32G32B32, R32G32B32A32 (with any data type: unorm, snorm, int, float, sRGB) |
| Packed | A2B10G10R10, B5G6R5, B6G5R5, A1B5G5R5, A5B5G5R1, A4B4G4R4, G4R4, R11G11B10F, E5B9G9R9 |
| Depth | Z16, Z24S8, S8Z24, X8Z24, ZF32, ZF32_X24S8 (R = depth) |
| Compressed (4x4 blocks) | BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC4, BC5 |

Other formats (ASTC, BC6H, BC7, ETC2) show a warning in Diagnostics and read as (0, 0, 0, 1).

## Instructions

| Instruction | GLSL | Operands |
| :--- | :--- | :--- |
| TEXS (scalar) | `texture`, `textureLod` with simple operands | Up to 4 sources: 1 = Ra; 2 = Ra, Rb; 3 = Ra, Ra+1, Rb; 4 = Ra, Ra+1, Rb, Rb+1. Results: 2 in Rd, Rd+1, the rest in Rd2, Rd2+1. A 4-bit "target" field picks 2D, 2D.LZ, 2D.LL, shadow, array, 3D or cube |
| TLDS (scalar) | `texelFetch`, `texelFetchOffset` | Same operand rules, integer coordinates |
| TEX | everything else (bias, offsets, arrays with LOD...) | Ra.. = [layer] + coordinates; Rb.. = [LOD or bias] [packed offsets, 4 bits each] [depth reference]; results in consecutive registers from Rd for the components in the 4-bit mask |
| TXD | `textureGrad` | Like TEX; Rb.. = derivatives interleaved (dx.x, dy.x, dx.y, dy.y...) |
| TXQ | `textureSize`, `textureQueryLevels` | Ra = level; results: width, height, depth or layers, number of levels |

Array layers come first as an integer (the compiler converts them with `F2I.U16`).

## Sampling

- **Level of detail.** On real hardware the implicit LOD comes from the difference between
  neighbouring pixels. NeXo runs one pixel at a time, so `texture()` uses level 0 (plus the
  bias). `textureLod`, `textureGrad` (LOD = log2 of the largest change in texels) and
  `texelFetch` choose levels correctly. Mipmap filter "point" rounds to the nearest level.
- **Filters:** point and bilinear (texel centres at +0.5). Integer formats are never filtered.
- **Wrap modes:** all eight, per texel (so bilinear filtering wraps each of the 4 texels).
- **Cube maps:** the largest axis of the direction picks the face (layer 0..5), with the
  OpenGL face table.
- **Shadow samplers:** the depth reference is compared with each texel before filtering
  (so bilinear gives a percentage).
- **texelFetch** outside the texture returns 0.

## Tests

| Test | Checks |
| :--- | :--- |
| `Texture_Nearest2DBlockLinear` | 8x8 block linear texture, every one of the 4096 pixels is the right texel |
| `Texture_BilinearPitchAndWrapModes` | 2x2 pitch texture, bilinear weights, clamp to edge vs repeat at the corners |
| `Texture_MipmapsLodAndGrad` | 3 levels with a 2-GOB block height: `textureLod` (incl. rounding and clamping) and `textureGrad` |
| `Texture_TexelFetch` | exact texels, level 1, `texelFetchOffset`, out of range = 0 |
| `Texture_ArrayLayersAndSize` | 2D array layers; `textureSize` and `textureQueryLevels` (TXQ) |
| `Texture_ShadowCompare` | ZF32 texture with a less-or-equal comparison |
| `Texture_InVertexShader` | texture read in the vertex shader |
| `Texture_FormatsAndBc1` | B5G6R5, half floats, snorm, BC1 colours and indices |
| `Texture_DescriptorDecodeAndLevelOffsets` | TIC fields and mip offsets of a 300x200 texture with 16-GOB blocks |

## Not yet

Implicit LOD from pixel derivatives (needs 2x2 pixel groups), anisotropic filtering, TLD4
(`textureGather`), images (`imageLoad/Store`), ASTC/BC6/BC7, multisample textures, seamless
cube edges, and the texture cache between draws (each draw decodes again).
