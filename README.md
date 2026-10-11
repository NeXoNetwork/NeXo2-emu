# NeXo 2 | Emulator for Nintendo Switch 2

NeXo 2 is an experimental, open-source Nintendo Switch 2 emulator written from scratch in C++20.
The CPU, the HLE kernel and the GPU are hand-written and documented, and every change is checked by
an automated test suite. It runs Switch homebrew (`.nro`) today, including 3D games; commercial
games are not supported yet.

Website: <https://nexonetwork.github.io/www/> · Build guide: [BUILDING.md](BUILDING.md) ·
Internals: [docs/07-nexo-internals/README.md](docs/07-nexo-internals/README.md)

## What runs

| Program | Type | Status |
| :--- | :--- | :--- |
| **NZ:P (Nazi Zombies Portable)** | Homebrew 3D game (SDL2 + OpenGL/mesa) | Playable, with sound (slow: software GPU) |
| **hbmenu** | Homebrew menu | Works: lists the SD card, icons, launches other `.nro` (chain loading) |
| **Homebrew App Store** | SDL2 app | Works: downloads the real catalogue over https |
| **deko3d examples** | Official 3D samples | 7 of 9 (tessellation and compute missing) |
| UMSDPong, NX-FixCheat | libnx homebrew | Work |
| GTA Chinatown Wars (port) | Homebrew port | Boots, asks for its data files |
| Commercial games | — | Not yet (needs NSO/NCA, audren, Vulkan) |

## Features

* **CPU (ARM64):** our own interpreter for all of ARMv8.2-A at EL0 (integer, FP with exact ARM rules,
  NEON, AES/SHA, CRC32, LSE atomics), 100 % identical to a reference ARM in differential fuzzing.
  **JIT** with dynarmic (~1 100 M instr/s). See [cpu-interpreter.md](docs/07-nexo-internals/cpu-interpreter.md), [jit.md](docs/07-nexo-internals/jit.md).
* **Threads:** 6 emulated cores, **each on its own host thread**, with real atomics
  (LDXR/STXR, CAS, LSE) and a shared exclusive monitor. See [threads.md](docs/07-nexo-internals/threads.md).
* **HLE kernel and services:** ~40 SVCs, HIPC/CMIF/TIPC with domains, and the services homebrew uses:
  `sm:`, `set`, `apm`, `appletOE`, `hid`, `time`, `fsp-srv` (SD card = a PC folder), `vi`, `nvdrv`,
  `pl:u` (system font), `psm`, `ts`, `csrng`, network and audio. See [ipc-and-services.md](docs/07-nexo-internals/ipc-and-services.md).
* **GPU (Maxwell):** GPFIFO, macros, DMA/2D engines, a shader interpreter and a multithreaded
  software rasterizer (textures BC1–BC5, mipmaps, MSAA, shared texture cache), running on **its own
  thread**. Vulkan backend in progress. See [gpu.md](docs/07-nexo-internals/gpu.md), [gpu-rasterizer.md](docs/07-nexo-internals/gpu-rasterizer.md).
* **Network:** host sockets (`bsd`), DNS (`sfdnsres`), `nifm`, and real **TLS** (`ssl`, mbedTLS) checked
  against the system's root certificates. See [network.md](docs/07-nexo-internals/network.md).
* **Audio:** `audout:u` played through SDL3 (48 kHz stereo). See [audio.md](docs/07-nexo-internals/audio.md).
* **Input:** PC keyboard and gamepads as Switch controllers. See [input.md](docs/07-nexo-internals/input.md).

## Tests

`nexo2_tests` runs 98 tests twice — with the interpreter and with the JIT (196 runs): CPU programs,
differential CPU/SIMD fuzzing, IPC and services, the NRO loader, threads and multicore, GPU
engines, shaders (fuzzed against a reference compiler), draws and textures, network and Vulkan.
They also pass under ASan/UBSan and TSan.

```
build.bat test
```

## Downloads

Every commit to `main` is built by GitHub Actions and published as a **prerelease**
(`v<version>-dev.<n>`, Windows x64). Official releases are published by hand.
Unzip, put your `.nro` files in `sdmc/switch` and open `NeXo2.exe`.

## Project structure

* `src/common/` — logging and global types.
* `src/core/arm64/` — interpreter and JIT (dynarmic).
* `src/core/memory/` — guest memory (atomic page tables) and region map.
* `src/core/loader/` — NRO loader.
* `src/core/hle/` — kernel (SVCs, threads, IPC) and system services (`services/`, `net/`).
* `src/video_core/` — GPU: command processor, engines, shaders, software rasterizer, textures, Vulkan.
* `src/main.cpp` — SDL3 + ImGui application.
* `tests/` — automated tests and the programs they run.
* `docs/` — hardware, OS and NeXo internals documentation ([docs/README.md](docs/README.md)).
* `externals/` — submodules: dynarmic, SDL3, ImGui, Vulkan-Headers, volk, mbedTLS.

## Roadmap

1. [x] Foundations — memory, NRO loader, HLE kernel, IPC
2. [x] Display and controllers
3. [x] Complete CPU — ARMv8.2 at 100 %, decode cache, JIT
4. [x] Threads — 6 cores on host threads, GPU on its own thread
5. [x] Software GPU — shaders, textures, 3D
6. [x] Network and https
7. [~] Audio — `audout:u` done; `audren:u` next
8. [~] Vulkan — device init done; Maxwell → SPIR-V translator next
9. [ ] Game formats — NSO, NCA, RomFS

## Legal

NeXo 2 is an independent research and learning project, not affiliated with or endorsed by Nintendo.
It does not include firmware, keys or games and does not help to obtain them. Use it with homebrew
or software you legally own. Licensed under the GPLv3 (see [LICENSE](LICENSE)).
