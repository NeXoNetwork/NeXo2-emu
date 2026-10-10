# 07 — NeXo internals

How NeXo 2 itself is built (our code, not the console).

| File | Content |
| :--- | :--- |
| [cpu-interpreter.md](cpu-interpreter.md) | ARM64 interpreter: structure, implemented instructions, floating point with ARM rules, tests, how to add an instruction, performance, decode cache |
| [jit.md](jit.md) | JIT with dynarmic: how it fits with the interpreter, page table, self-modifying code, tests, speed |
| [cpu-fuzzing.md](cpu-fuzzing.md) | Random instructions compared against a reference ARM: how it works, results, known QEMU differences, usage |
| [nro-loader-and-hle.md](nro-loader-and-hle.md) | NRO loader, process memory layout, Homebrew ABI, HLE kernel SVCs |
| [ipc-and-services.md](ipc-and-services.md) | IPC (HIPC/CMIF/TIPC, domains), all services (sm:, applet, hid, time, fs/SD card...), how to add a service |
| [threads.md](threads.md) | Threads, scheduler (6 cores: time-sliced or one host thread each), mutex/condvar protocol, shared memory and atomics |
| [input.md](input.md) | Controllers: hid shared memory layout, LIFOs, PC keyboard/gamepad mapping |
| [gpu.md](gpu.md) | GPU: nvhost devices, channels, pushbuffers, engines (3D clears, DMA, 2D, inline), macros, syncpoints and fences, the GPU thread |
| [gpu-shaders.md](gpu-shaders.md) | Maxwell shader decoder and interpreter, shader environment, random-program tests with uam |
| [gpu-rasterizer.md](gpu-rasterizer.md) | Draws: vertex fetch, clipping, viewport, culling, rasterization rules, depth, blending |
| [gpu-vulkan.md](gpu-vulkan.md) | GPU phase 3: Vulkan device (volk, no SDK), plan for the SPIR-V translator and Vulkan draws |
| [gpu-textures.md](gpu-textures.md) | Textures: TIC/TSC descriptors, formats (incl. BC1-5), block linear and mipmaps, TEX/TEXS/TLDS/TXD/TXQ, filtering |
| [display.md](display.md) | vi + nvdrv: binder buffer queue, GraphicBuffer layout, block-linear deswizzle, the "Pantalla" window |

## Test suite

`nexo2_tests` (94 tests, each run with the interpreter and with the JIT) covers every page above: CPU programs, SIMD differential tests, CPU fuzzing,
the NRO loader, IPC, the libnx start-up services, display (deswizzle, GraphicBuffer parsing,
pixel formats), GPU (shaders, draws, textures), SD card paths, controller LIFOs, decode cache and threads. Run it after any change:
`build\Release\nexo2_tests.exe`.
