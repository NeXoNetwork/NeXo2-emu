# GPU with Vulkan (phase 3)

Phase 2 draws everything on the CPU ([gpu-rasterizer.md](gpu-rasterizer.md)). It is exact but
slow: a few frames per second for simple 3D scenes. Phase 3 does the same work on the PC's
graphics card through Vulkan. The software renderer stays as the reference, and the tests can
compare the two.

## Plan

| Step | What | Status |
| :--- | :--- | :--- |
| 3a | Open Vulkan without a window: pick the GPU, one queue, memory, buffers, one-shot command submission | done |
| 3b | Maxwell shader → SPIR-V translator, checked against the interpreter with the random programs | next |
| 3c | Draws: 3D engine state → Vulkan pipelines, render targets and textures as Vulkan images, sync with guest memory | |
| 3d | Choose the renderer in the UI (software / Vulkan) | |

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

glslang is only used for these test shaders. Game shaders will be translated by NeXo itself
(step 3b).
