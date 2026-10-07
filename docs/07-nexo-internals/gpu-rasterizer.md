# GPU draws (software rasterizer)

When a program draws, the 3D engine (`Maxwell3D`) calls `SoftwareRasterizer::Draw` with its
registers. Everything happens on the CPU, synchronously, and the result is written to the
render target in the program's memory. From there the existing `vi` path shows it in the
"Pantalla" window ([display.md](display.md)).

Files: `src/video_core/rasterizer.hpp/.cpp`, `tests/gpu_draw_tests.cpp`, `tests/gpu_draw_fixture.hpp`,
`tests/programs/nro_triangle`.

## Which methods start a draw

deko3d's `Draw` macro sends `VertexBeginGl` (0x586, topology + instance flags),
`DrawArraysFirst` (0x35D), then `DrawArraysCount` (0x35E) and `VertexEndGl` (0x585).
Writing the count is what triggers the draw. Indexed draws use `DrawElementsFirst/Count`
(0x5F7/0x5F8) with the index buffer at 0x5F2.. and `VertexIdBase` (0x446) as the vertex offset.
The `DRAW_*_BEGIN_END_INSTANCE_*` methods (0x485/0x486, 0x5F9..0x5FE) are supported too.

Register numbers are method offset / 4, with names from NVIDIA's public class header
`clb197.h` (open-gpu-doc) and deko3d's `engine_3d.def`.

## Pipeline

1. **Programs.** `SetProgramRegion` (0x582) + `SetProgram[stage].Offset` (0x801 + 0x10*stage).
   Stage 1 (VertexB) and 5 (Fragment) are supported. Draws with tessellation, geometry or
   VertexA shaders are skipped with a warning.
2. **Constant buffers.** `ConstbufSelector` (0x8E0..0x8E2) chooses an address and
   `Bind[stage].Constbuf` (0x904 + 8*stage) puts it in slot `c[index]`. The rasterizer copies
   each one the first time a shader reads it during the draw.
3. **Vertex fetch.** 32 attributes (0x458: stream, offset, size, type, BGRA swap) read from
   16 vertex streams (0x700: stride, enable, address; 0x620: per instance; divisor). Formats:
   1-4 components of 8/16/32 bits, 10-10-10-2; snorm, unorm, sint, uint, scaled, float
   (including half floats).
4. **Vertex shader**, once per unique index (with a cache, as the GPU does).
5. **Primitive assembly.** Triangles, strips (with alternating winding), fans, quads, quad
   strips, polygons, and primitive restart. Points and lines: not yet.
6. **Clipping** in clip space (Sutherland-Hodgman): near (`z >= -w` or `z >= 0` depending on
   `SetDepthMode`, 0x35F), far, `w > 0` and a guard band of 8x the viewport. All attributes
   are interpolated for the new vertices.
7. **Viewport.** `ndc = xyz / w`, then `window = scale * ndc + offset` (0x280). deko3d uses a
   negative Y scale, so NDC +Y is the top of the image.
8. **Facing and culling.** Winding is measured in window coordinates. With `SetWindowOrigin.FlipY`
   (0x4EB bit 4) off, a visually counter-clockwise triangle has negative area (Y goes down).
   `SetFrontFace` (0x647: 0x900 CW, 0x901 CCW), `CullFaceEnable`/`SetCullFace` (0x646/0x648).
9. **Rasterization.** Vertices are snapped to 1/256 pixel and edge functions are evaluated
   in 64-bit integers, so shared edges give exactly opposite values: no gaps and no double
   pixels (tested with a quad made of two triangles). Pixel centres are at +0.5, with the
   top-left rule for pixels exactly on an edge. The area is limited to the render target,
   viewport 0 (0x300), the screen scissor (0x3FD) and scissor 0 (0x380).
10. **Pixel shader** per covered pixel: barycentrics give depth, 1/w and the attributes (IPA).
    Both shaders can read textures ([gpu-textures.md](gpu-textures.md)).
11. **Depth test** (0x4B3 enable, 0x4BA write, 0x4C3 function). It runs before the pixel shader
    when the shader does not write depth. Formats: Z16, Z24S8, S8Z24, X8Z24, Z32F, Z32F_X24S8.
12. **Colour output.** Render targets from `RenderTargetControl` (0x487). With MRT, output *k*
    goes to target *k*, otherwise output 0 goes to all. Write masks (0x680), blending (0x4D8
    enable; global 0x4CF.. or per target 0x780..; OpenGL and Direct3D factor values; add,
    subtract, reverse subtract, min, max; constant colour). Each row is read once, modified
    and written back (pitch or block linear).

## Multisampling (MSAA)

With `MultisampleMode` (0x574) set, render targets and depth buffers are stored in samples
(4x MSAA = 2x2 samples per pixel, so a 1280x720 target is 2560x1440), while viewports and
scissors stay in pixels. NeXo shades each pixel once and writes the result to all of its
samples. Edges are not smoothed, but the program's resolve step (a 2D engine blit that
averages the samples) gives the right image. Clears follow the same rule.

## Tests

`tests/gpu_draw_tests.cpp` uses real shaders from `tests/shaders`, compiled with uam:

| Test | Checks |
| :--- | :--- |
| `Draw_TriangleColorsAndCoverage` | colours at the corners, 1/3 each at the centroid, exact area (2048 px), background untouched |
| `Draw_CullingAndWinding` | deko3d defaults (front = CCW, cull back): CCW drawn, CW culled |
| `Draw_IndexedStripLinearTargetNoGaps` | u16 index buffer with an offset, triangle strip, pitch-linear target: exactly 32x32 pixels |
| `Draw_DepthTest` | two overlapping triangles in both orders: the near one always wins, depth buffer value |
| `Draw_DiscardAndUniformBlend` | `discard` (KIL); colour from a uniform buffer in the pixel shader, 50 % alpha blending |
| `Draw_PerspectiveFromUniformMatrix` | matrix from a uniform buffer, w = 2 halves the triangle, perspective-correct colours |
| `Texture_*` | textured draws, see [gpu-textures.md](gpu-textures.md) |
| `Draw_NroTriangle` | `triangle.nro`: a homebrew that sets everything up through `nvdrv` like deko3d, draws and checks its own pixels |

## Speed

The pixel shader is interpreted for each pixel, so this is the slow part. Two things help:

- **Threads.** Vertices are shaded on one thread; then the screen is split into bands of 4
  rows, and each CPU thread takes every N-th band and walks *all* triangles of the draw in
  order. A pixel is always written by the same thread and in the same order as on the GPU,
  so depth tests and blending give exactly the same result as with one thread. Each thread
  has its own interpreter, its own copy of the decoded shader and its own texture cache;
  constant buffers and the memory pages of the targets are loaded before the threads start.
  Draws smaller than about 1000 pixels stay on one thread. `NEXO2_GPU_THREADS=1` (environment
  variable) forces one thread, to compare or debug.
- **Fast clears.** Clearing a whole surface with one colour writes the memory in large
  blocks (the block linear order doesn't matter when every pixel is the same).

Rough numbers on a 2-core cloud machine: deko3d's lit teapot with 4x MSAA went from 0.6 s
to 0.23 s per frame. Expect more with more cores; the Vulkan backend (phase 3) is the real fix.

## Not yet

Points and lines, stencil, per-sample coverage (antialiased edges), tessellation
and geometry shaders, transform feedback, queries.
