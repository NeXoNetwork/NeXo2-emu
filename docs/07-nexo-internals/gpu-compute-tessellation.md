# Compute and tessellation

Both run on the CPU with the shader interpreter, like the vertex shader ([gpu-shaders.md](gpu-shaders.md)).
Pixels still go through Vulkan or the software rasterizer.
All nine deko3d examples now run, including 05 (tessellation in line mode) and 09 (compute + wide line strip).

## Compute (`video_core/compute.*`)

The compute engine (class 0xB1C0) does not take its parameters as registers. They come in a **QMD**, 0x100 bytes in GPU memory:

- `SendPcasA` (0xAD) gives the QMD address >> 8.
- `SendSignalingPcasB` (0xAF) launches it.

| QMD word | Field |
| :--- | :--- |
| 8 | program offset inside `SetProgramRegion` (0x582). Compute code has **no SPH**: `ShaderProgram(addr, read, false)` |
| 12, 13 | number of workgroups: x; y (low 16 bits), z (high 16 bits) |
| 17 | shared memory per workgroup (18 bits) |
| 18 (high 16 bits), 19 | threads per workgroup: x; y, z |
| 20 | mask of valid constbufs c[0]..c[7] |
| 29 + 2i, 30 + 2i | constbuf i: address low; address high (8 bits) and size (bits 15..31) |

How a dispatch runs:

- Workgroups run one after another.
- Inside a workgroup, every thread has its own `ShaderInterpreter` (registers, stack, local memory) with `group_mode` on.
- A `BAR.SYNC` returns `Result::barrier`. The loop moves on to the next thread and later continues with `Resume`.
  So every thread reaches the barrier before any thread goes past it, as on the GPU.
- Thread ids (`S2R`):
  - `tid`, `tid.x/y/z`, `ctaid.x/y/z`;
  - `laneid` = linear id % 32;
  - lane masks.
- Textures use the compute engine's own pools: 0x557/0x55D and the bindless constbuf 0x982.

Memory:

- **Global memory** (`LDG/STG`, generic `LD/ST`, `ATOM`, `RED`) goes through `GpuMemoryManager`, so Vulkan images are written back first when needed.
- When the dispatch ends, the written range is reported with `Gpu::OnGpuWrite`. The texture cache and Vulkan then see the new data.
- **Shared memory** is one buffer per workgroup, cleared at its start.
- deko3d puts the storage buffers in c[0] at 0x140 + 16·i (address, size). The shader reads them itself with `LDC`/constbuf operands.

Stats: `dispatches`, `compute_threads`.

## Tessellation (`video_core/tessellation.*`)

It runs inside `SoftwareRasterizer::RunVertexStage` when the topology is 14 (patches), after the vertex shader. The output is ordinary triangles, so the software rasterizer and Vulkan draw them the same way.

1. **Patches:** `TessellationPatchSize` (0x373) vertices each.
2. **Control shader** (stage 2, optional):
   - It runs once per output control point. The count is SPH word 2 bits 24..31 (`layout(vertices = N)`).
   - `invocation_id` = the thread; `invocation_info` low byte = vertices per patch.
   - `ALD` with a vertex register (bits 39..46) reads that input vertex. With bit 32 (`o`) it reads an output control point; with bit 31 (`p`) a patch attribute.
     `ISBERD` is the identity (the index is already the vertex).
   - Patch attributes: outer levels at a[0x00..0x0C], inner at a[0x10..0x14].
   - Threads run as a group (barriers work like compute).
   - Without a control shader, the control points are the input vertices and the levels come from 0x0C9..0x0CE.
3. **Tessellator** (`Tessellate`). Register 0x0C8:
   - domain (0 isolines, 1 triangle, 2 quad);
   - spacing (0 equal, 1 fractional odd, 2 fractional even);
   - bit 8 = clockwise.

   How it divides each domain:
   - **Triangles:** concentric rings, each (n − 2k)/n the size of the outer one around the centre. The outer ring uses the three outer levels (edge u=0 → level 0, v=0 → 1, w=0 → 2). Neighbouring rings are stitched by walking both edges; the centre is a point (even n) or a triangle (odd n).
   - **Quads:** the same with rectangles (u=0 → level 0, v=0 → 1, u=1 → 2, v=1 → 3). What is left in the centre is filled with a grid.
   - **Isolines:** outer[0] lines of outer[1] segments.

   Rules:
   - A patch with an outer level ≤ 0 is dropped.
   - All levels 1 gives a single triangle or quad.
   - Otherwise an inner level of 1 counts as 1 + ε (2, or 3 for odd spacing).
   - Fractional spacing rounds to the right odd/even count but spaces points evenly. Point positions are not exact for fractional levels.
4. **Evaluation shader** (stage 3):
   - It runs once per domain point.
   - u, v are in a[0x2F0], a[0x2F4]; control points via `ALD` with a vertex register; patch attributes via the `p` bit.
   - It writes outputs exactly like a vertex shader.

## Points, lines and polygon mode (`WidePrimitives` in rasterizer.cpp)

- Points (0), lines (1), line loop (2), line strip (3) and the adjacency topologies (10–13) are turned into triangles in clip space.
  - **Lines:** a rectangle `LineWidthAliased` (0x4ED) pixels wide, perpendicular to the line on screen. They are first cut at w > 0.
  - **Points:** a square of `PointSpriteSize` (0x546), or the shader's a[0x6C] when 0x644 bit 0 is set. The corners get gl_PointCoord in a[0x2E0/0x2E4].
- These triangles are never culled (`VertexStage::no_cull`).
- **Polygon mode** (0x36B front / 0x36C back: 0x1B00 points, 0x1B01 lines, 0x1B02 fill): after face culling, each triangle becomes its edges or its vertices. Example 05 uses this.

## Tests (`tests/gpu_compute_tess_tests.cpp`)

The shaders are in `tests/shaders`, compiled with uam by `tools/gen_shader_bins.py` (now also `.comp/.tesc/.tese`).

- `Compute_SharedMemoryBarrierAtomics`: 3 workgroups × 64 threads.
  - Each thread writes `i² + 1000·group` to shared memory. After `barrier()` it reads its mirror thread's value, so the result is wrong without real barriers.
  - All threads also do `atomicAdd` on one counter.
- `Tess_DomainShapes`:
  - known counts (1 triangle, 2 for a quad, 13 for level 3, 12 isoline segments, dropped patch);
  - for many level combinations (triangles and quads, all three spacings): the triangles cover exactly the domain area and all turn the same way (also clockwise).
- `Draw_TessellationMatchesPlainTriangle`: a TCS (levels 4, 2, 3 / 3, with a barrier) and a TES that only interpolates.
  - The image matches the untessellated triangle within 2 levels per channel.
  - The triangle count matches `Tessellate`.
  - Also runs with Vulkan.
- `Draw_WideLinesPointsAndWireframe`: a 48×4 line, two 6×6 points, and a triangle in line mode.
