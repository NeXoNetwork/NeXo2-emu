# GPU with Vulkan (phase 3)

Phase 2 draws everything on the CPU ([gpu-rasterizer.md](gpu-rasterizer.md)). It is exact but
slow: a few frames per second for simple 3D scenes. Phase 3 does the same work on the PC's
graphics card through Vulkan. The software renderer stays as the reference, and the tests can
compare the two.

## Plan

| Step | What | Status |
| :--- | :--- | :--- |
| 3a | Open Vulkan without a window: pick the GPU, one queue, memory, buffers, one-shot command submission | done |
| 3b | Maxwell shader → SPIR-V translator, checked against the interpreter with the random programs | done |
| 3c | Draws: 3D engine state → Vulkan pipelines, render targets and textures as Vulkan images, sync with guest memory | done (first version) |
| 3d | Choose the renderer in the UI (software / Vulkan) | done |
| 3e | Vertex shaders on the GPU, MSAA, depth-compare and integer textures, less copying to memory | next |

## Dependencies

Two git submodules, no Vulkan SDK needed:

- `externals/Vulkan-Headers`: Khronos' official headers.
- `externals/volk`: loads the Vulkan driver (`vulkan-1.dll` / `libvulkan.so.1`) at startup and
  gets every function pointer from it. `volk.c` is compiled into `nexo2_core` with
  `VK_NO_PROTOTYPES`.

CMake option `NEXO2_ENABLE_VULKAN` (ON by default). If the submodules are missing, NeXo builds
without Vulkan (`NEXO2_HAS_VULKAN` undefined) and keeps the software renderer.

## Device (`src/video_core/vulkan/vk_device.hpp/.cpp`)

`Vulkan::Device::Init()`:

1. `volkInitialize()`. If it fails, there is no Vulkan driver on this PC.
2. Instance with the highest API version up to 1.3 (at least 1.1 is required). With
   `NEXO2_VK_VALIDATION=1` (environment variable) it enables `VK_LAYER_KHRONOS_validation` if
   installed, and validation messages go to `nexo2.log`.
3. GPU choice: discrete > integrated > virtual > CPU (software), among the GPUs with a queue
   that does graphics and compute. `NEXO2_VK_DEVICE=n` forces GPU number *n* (the log lists them).
4. Logical device with that queue and the optional features we will need if the GPU has them
   (BC texture compression, stores from vertex/fragment shaders, independent blend, depth clamp...).

Helpers: `FindMemoryType`, `CreateBuffer` (device-local or host-visible and mapped),
`Submit(lambda)` (records a command buffer, submits it and waits).

The Diagnostics window shows `Vulkan Core: <GPU> (Vulkan x.y.z, driver ...)` in green, or the
reason in red.

## Tests

`tests/vulkan_tests.cpp`. Without Vulkan they print the reason and pass (nothing to check).
In the cloud they run on lavapipe, Mesa's software Vulkan driver.

| Test | Checks |
| :--- | :--- |
| `Vulkan_DeviceInit` | device and queue; `vkCmdFillBuffer` on the GPU, read back on the CPU |
| `Vulkan_ComputeSmoke` | a real compute shader (`tests/shaders/vulkan/smoke.comp`, compiled with glslang by `tools/gen_vk_test_spirv.py`) on 256 values |

glslang is only used for these test shaders. Game shaders are translated by NeXo itself
(step 3b, below).

## Shader translator (step 3b)

`src/video_core/shader_spirv.hpp/.cpp` turns a `ShaderProgram` (the same decoded Maxwell code the
interpreter runs) into a SPIR-V 1.3 module. `src/video_core/spirv_builder.hpp` is a tiny SPIR-V
writer: sections, ids, types and constants without duplicates.

**Control flow.** Maxwell code jumps freely (BRA/JMP, and SSY/SYNC, PBK/BRK, PCNT/CONT, CAL/RET
with a stack), but SPIR-V only accepts properly nested loops and ifs. The translator:

1. Walks the code from offset 8 and finds every *block start*: branch and stack targets, the
   instruction after a branch, an EXIT or a predicated jump, and return addresses.
2. Emits one loop around a switch on a `pc` variable. Each block is a `case`; at its end it stores
   the next `pc` (fall-through or jump target). EXIT clears a `running` flag that the loop's
   continue block checks. An unknown `pc` stops the program instead of spinning forever.
3. The control stack is two small arrays (kind and target) and a stack pointer. SSY/PBK/PCNT/CAL
   push; SYNC/BRK/CONT/RET pop until they find their kind, like the interpreter.

Registers (r0–r254), predicates (p0–p6) and the condition codes are function variables; the
drivers turn them into real registers.

**Instructions.** Every instruction does the same arithmetic as `shader_exec.cpp`: flush to zero
on the bits, `.SAT` with `NClamp` (NaN → 0), FMUL/FFMA `.FMZ` (0 × anything = 0), F2I limits and NaN
→ 0, 64-bit integer math for IADD.CC/.X/.SAT, IMUL and IMAD (needs `shaderInt64`), BFE/BFI edge cases,
FLO, XMAD modes, LDC with out-of-range reads giving 0, local memory (LDL/STL, 32/64/128 bits).
Not yet: textures (TEX/TEXS/TLDS/TXD/TXQ), F2I/I2F with 64-bit values, 8/16-bit local memory.
Anything not translatable returns an error, and that shader will keep using the software path.

**Stages.**

- `ComputeTest` (tests): constant buffer *i* is a storage buffer at binding *i*, attributes read
  with ALD/IPA come from binding 20 and AST writes go to binding 21.
- `Fragment` (step 3c): see below.

Vertex shaders are not translated: they still run on the CPU (see "Draws").

**Fragment stage.** Before emitting code the translator scans the program:

- Every IPA with a fixed address becomes an input (one `vec4` per 16-byte attribute slot and
  interpolation mode). Linear IPA → `NoPerspective` input; flat → `Flat` `uvec4` (bits unchanged);
  perspective → a normal (perspective-correct) input multiplied by `gl_FragCoord.w`, which gives
  exactly what IPA returns (value / w interpolated on screen; the program multiplies by w after).
  `a[0x70..0x7C]` is `gl_FragCoord`, `a[0x3FC]` is `gl_FrontFacing`. ALD reads the same two.
- Colour outputs: at the end, `R0..` go to locations 0..n-1 following the header's output map (as in
  the software renderer), as `vec4`, `ivec4` or `uvec4` depending on the render target format. The
  depth output (if the header has one) goes to `gl_FragDepth`, clamped to 0..1. KIL is `OpKill`.
- `S2R SR_Y_DIRECTION` and the coordinate scale of textures with unnormalized coordinates come from
  a small storage buffer at binding 18 (`DRAW_INFO`).
- Textures: each texture slot (`c[TextureConstbuf][slot * 4]`) is a combined image sampler at
  binding 32 + k. TEX/TEXS/TXD sample (implicit lod, `.LZ`, `.LL`, `.LB`, gradients, AOFFI offsets),
  TLDS is `OpImageFetch` (with offsets added to the coordinates), TXQ is `OpImageQuerySizeLod` +
  `OpImageQueryLevels`. 1D, 2D, 2D arrays, 3D and cubes. Not yet: depth compare, cube arrays.

`BuildPassthroughVertexShader` writes the matching vertex shader: location 0 = position, location
1 + i = input i, copied to the outputs.

**Tests** (`tests/gpu_spirv_tests.cpp`):

- `Spirv_TranslatesFuzzPrograms`: the 300 random programs of `shader_fuzz.bin` translate.
  `NEXO2_SPIRV_DUMP=dir` writes them as `.spv`; all pass `spirv-val --target-env vulkan1.1`.
- `Spirv_FuzzOnVulkan`: each one runs as a compute shader on the Vulkan device and its 16
  outputs are compared with the expected values. 300 of 300 (4 800 values) match on lavapipe.
- `Spirv_TranslatesFragmentShaders`: the pixel shaders of the draw tests (compiled with uam)
  translate as `Fragment` and pass `spirv-val` (the shadow one must fail for now).
- `NEXO2_TEST_FILTER=Spirv` runs only these tests (any part of a test name works).

## Draws (step 3c)

`src/video_core/vulkan/vk_rasterizer.hpp/.cpp`, class `VulkanRasterizer`. The 3D engine calls it
for each draw and each clear; if it returns false, that draw is done by the software renderer (the
reason goes to the log once).

**Per draw:**

1. Same checks and state as the software renderer: render targets, depth, the drawable box
   (viewport, screen scissor, scissor 0). Anything unsupported (MSAA, tessellation/geometry, an
   untranslatable shader, depth-compare or integer textures) → software.
2. The pixel shader is translated (cached by address and options; the program bytes are compared
   on each use, in case the game uploaded another one at the same address).
3. **The vertex shader runs on the CPU**, with the same code as the software renderer
   (`SoftwareRasterizer::RunVertexStage`: indices, the interpreter, primitive assembly into a
   triangle list). Vertices are few; pixels are what costs.
4. Each vertex goes to the GPU as `position + one vec4 per pixel-shader input`. The position is
   rewritten so that a Vulkan viewport covering `[0, width) × [0, height)` lands on the same pixel
   as Maxwell's viewport transform: `x' = 2(sx·x + ox·w)/width − w`, same for y, `z' = sz·z + oz·w`,
   `w' = w`. It is a linear change of the clip coordinates, so Vulkan's clipping and
   perspective-correct interpolation stay right. The "provoking" vertex (flat values) is put first.
5. Constant buffers used by the shader are copied now (the program may change them after) into
   a host-visible upload buffer and bound as storage buffers. Textures (below) are prepared.
6. Pipeline from the state (cached): blend per target (OpenGL and Direct3D enum values), write
   masks, depth test/write/function (as in OpenGL and Vulkan, no depth test = no depth write), culling and front face
   (with FlipY the screen is upside down, so the winding flips). Viewport, scissor and blend
   constants are dynamic. Rendering uses `vkCmdBeginRendering` (dynamic rendering: Vulkan 1.3 or
   `VK_KHR_dynamic_rendering`); consecutive draws to the same targets stay in one rendering pass.

**Render targets and memory.** Guest memory stays the reference. Each render target or depth
buffer is a Vulkan image (colour: a Vulkan format with the same bytes; depth: always `D32_SFLOAT`,
converted). An image is:

- **uploaded** from memory (de-swizzled from block linear) when created, when another part of the
  GPU wrote that memory (`OnGpuWrite`), or when the CPU changed it between submits (a hash of the
  memory is compared once per submit). A full clear skips the upload.
- **written back** to memory (`Download`: copy to a host buffer, swizzle, `WriteBlock`) at the end
  of every GPFIFO submit for colour (that is what gets presented), and for everything when someone
  is about to read or write that memory: `GpuMemoryManager` calls `BeforeAccess` on every
  `ReadBlock`/`WriteBlock`, so DMA, 2D blits, texture decoding and inline uploads always see the
  latest pixels. Before a software draw its targets are written back too (it uses direct pointers).

**Textures** are decoded on the CPU by the shared `TextureCache` (as 4 × 32-bit values per texel),
uploaded as `R32G32B32A32_SFLOAT` images with all their mip levels (or RGBA16F if the GPU can't
filter 32-bit floats), with the TIC swizzle as the view's component mapping. A level is uploaded
again only when the cache returns a new decode (its memory changed). Samplers come from the TSC
(filters, wrap modes, lod bias/clamp; border colours rounded to black, transparent or white). The
level of detail is computed by the GPU from the real derivatives, so mipmapping is better than in the
software renderer (which always uses level 0 for implicit lod).

**Render targets read as textures.** If a texture is exactly a render target that is already a
Vulkan image (same address, size, layout and format), the shader samples that image directly: no
write-back, no CPU decode, no upload. **2D engine copies** (`Fermi2D` blits) whose source is a Vulkan
image are done with `vkCmdBlitImage` (flips included). NZ:P copies the whole screen to a texture
every frame this way; doing it on the GPU took it from ~6 to ~20 fps on lavapipe. Images that are
only blit destinations are not written back at the end of each submit (only when someone reads
their memory).

**Timing.** `VulkanRasterizer::GetStats()` has the time per part (draw, CPU vertex shader, textures,
render target checks, waiting for the GPU, writing back); Diagnostics shows it in ms per second.
`NEXO2_TEX_DEBUG=1` prints every texture decode (to see what gets decoded again and again).

**Clears** (`ClearSurface`) of a target that is already on the GPU, or of a whole surface, are
`vkCmdClearAttachments`; partial channel masks and stencil-only clears go to software.

**Choosing the renderer.** Settings → Graphics → "Motor de dibujo": Vulkan (default) or Software;
`NEXO2_RENDERER=vulkan|software` overrides it. If Vulkan can't start, NeXo draws by software and says
why in the log. `Gpu::SetRenderer`, `Gpu::RendererName`, `Gpu::FlushRenderer`. The status bar shows
"GPU: Vulkan" and Diagnostics shows how many draws went through Vulkan.

**Tests.** `Vulkan_DrawAndTextureTests` runs every `Draw_*` and `Texture_*` test again with the Vulkan
renderer (same pixel checks). On lavapipe: all pass, 32 of 33 draws through Vulkan (the
depth-compare one falls back). NZ:P's menu and game render the same as with the software renderer and
about 6× faster even on lavapipe (a CPU implementation of Vulkan).
