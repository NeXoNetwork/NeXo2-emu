# NeXo 2 | Emulator for Nintendo Switch 2

NeXo 2 is an open-source research project focused on the study and analysis of the Nintendo Switch 2 (T239) hardware architecture. The project aims to document the system components and experiment with hybrid compatibility layers (a mix of HLE and LLE) using C++20..

## Project Objectives

* **Architecture Documentation:** Comprehensive analysis of the Nvidia T239 (Drake) SoC and its ARMv8/v9 instruction set extensions.
* **Memory Management:** Research into LPDDR5X memory layouts and virtual address space orchestration.
* **Binary Translation:** Implementation of high-performance translation layers, exploring both interpretive and Dynamic Recompilation (JIT) methods.
* **Graphics Subsystem:** Experimental mapping of NVN2 API calls to Vulkan 1.3 primitives.

## Technical Specifications

| Component | Specification |
| :--- | :--- |
| SoC | Nvidia T239 (Custom Ampere) |
| CPU | 8x ARM Cortex-A78C |
| GPU | GA10B (1536 CUDA Cores) |
| Graphics API | Vulkan 1.3 / NVN2 |
| Architecture | ARMv8.2-A / ARMv9-A |

## Project Structure

* `src/common/`: Utilities, thread-safe logging, and global types.
* `src/core/arm64/`: CPU execution engine and state management.
* `tests/`: Automated tests (CPU, loader, IPC, services, display, input) and the programs they run.
* `src/core/memory/`: Virtual Memory Manager (VMM) and memory region map.
* `src/core/loader/`: Executable loaders (NRO).
* `src/core/system.hpp`: `System` class tying memory, CPU, kernel and loader together.
* `src/core/hle/`: High-Level Emulation: kernel SVCs, IPC, system services (`services/`), display and input.
* `src/video_core/`: Vulkan implementation and hardware renderer.
* `docs/`: Hardware and OS documentation — start at [docs/README.md](docs/README.md).
* `externals/`: Third-party dependencies (dynarmic, SDL3, ImGui...).

## Development Roadmap (2026)

## Current Phase:
  * **Homebrew running** — libnx homebrew boots, draws on screen and reads the controller.
    Current focus: **speed** (JIT done; next: threads on several host cores).

### Phase 1: Core Infrastructure
- [x] **Logging Framework:** Thread-safe logger to console and `nexo2.log` (`src/common/logger.hpp`).
- [~] **Memory Management Unit:** Paged guest memory (4 KB pages, allocated on demand) with a Horizon-style region map. 64 KB pages and permission checks pending.
- [ ] **Virtual File System (VFS):** Initial support for parsing NCA and HFS2 container formats.
- [x] **NRO Loader:** Loads homebrew `.nro` files (segments, memory map, NACP title). See [docs/07-nexo-internals/nro-loader-and-hle.md](docs/07-nexo-internals/nro-loader-and-hle.md).
- [ ] **Command Line Interface:** Robust argument parsing for debugging and trace orchestration.

### Phase 2: CPU Emulation (ARM64)
- [x] **State Management:** Implementation of the ARM64 register set (X0-X30, SP, PC, and PSTATE).
- [x] **Instruction Decoder:** All of ARMv8.2-A for EL0: integer, scalar FP (half/single/double with exact ARM NaN, FPCR and FPSR rules), all Advanced SIMD/NEON groups, crypto (AES/SHA1/SHA256), CRC32, LSE atomics. 100 % of ~15 600 random valid instructions match a reference ARM (QEMU). See [cpu-interpreter.md](docs/07-nexo-internals/cpu-interpreter.md) and [cpu-fuzzing.md](docs/07-nexo-internals/cpu-fuzzing.md).
- [x] **Execution Loop:** Basic Fetch-Decode-Execute cycle for architectural verification.
- [x] **CPU Tests:** `nexo2_tests` runs hand-written ARM64 programs and clang-compiled C code.
- [x] **JIT Integration:** dynarmic translates ARM64 to x86-64 (~1 100 M instr/s, x6 the interpreter); every test also runs under the JIT. See [jit.md](docs/07-nexo-internals/jit.md).

### Phase 3: Graphics Subsystem (Vulkan)
- [~] **GPU (Maxwell) phase 1:** nvhost devices, channels, GPFIFO/pushbuffers, macros (MME), clears, DMA copies, 2D blits, syncpoints and semaphores. No shaders yet. See [gpu.md](docs/07-nexo-internals/gpu.md).
- [ ] **Vulkan Backend:** Initialization of the Vulkan 1.3 instance and physical device selection.
- [x] **Windowing Integration:** SDL3 window with ImGui; the "Pantalla" window shows what the program presents.
- [x] **Software framebuffer:** `vi` + `nvdrv` (nvmap, binder buffer queue, block-linear deswizzle). libnx console homebrew is displayed. See [docs/07-nexo-internals/display.md](docs/07-nexo-internals/display.md).
- [ ] **VMA Integration:** Implementation of the Vulkan Memory Allocator for emulator-to-GPU mapping.
- [ ] **Shader Pipeline:** Preliminary research into Ampere microcode-to-SPIR-V translation.

### Phase 4: OS Kernel & Services (HLE)
- [~] **Supervisor Call Dispatcher:** HLE kernel with ~30 SVCs (memory, events, shared/transfer memory, IPC, single-thread mutex/condvar, GetInfo...). Homebrew ABI loader config. See [docs/07-nexo-internals/nro-loader-and-hle.md](docs/07-nexo-internals/nro-loader-and-hle.md).
- [~] **Service Infrastructure:** IPC (HIPC/CMIF/TIPC, domains), handle table, events, shared memory. Services: `sm:`, `set:sys`, `apm`, `appletOE`, `hid`, `time`, `fsp-srv` (SD card on a PC folder), `vi`, `nvdrv` (screen), controllers via hid shared memory (PC keyboard and gamepad, see [docs/07-nexo-internals/input.md](docs/07-nexo-internals/input.md)). See [docs/07-nexo-internals/ipc-and-services.md](docs/07-nexo-internals/ipc-and-services.md).
- [~] **Scheduler:** Real guest threads on 6 emulated cores (time-sliced on one host thread), libnx-compatible mutex and condition variables, deadlock detection. Parallel host threads next. See [docs/07-nexo-internals/threads.md](docs/07-nexo-internals/threads.md).

## Development Status

NeXo 2 runs Switch homebrew built with libnx (`.nro`): text console and software framebuffer
in the "Pantalla" window, PC keyboard/gamepad as controller, a PC folder as SD card. It is slow
(pure interpreter) and cannot run commercial software. Tested: NX-FixCheat, UMSDPong.
How to build and run: [BUILDING.md](BUILDING.md). This repository is maintained strictly for educational and research purposes.

## Contribution

Technical contributions regarding C++20, ARM64 assembly, and Vulkan API implementation are welcome via Pull Requests.

## License

This project is distributed for educational purposes and hardware preservation research.