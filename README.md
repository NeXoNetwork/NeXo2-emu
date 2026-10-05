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
* `tests/`: CPU tests and the ARM64/C test programs they run.
* `src/core/memory/`: Virtual Memory Manager (VMM) and memory region map.
* `src/core/loader/`: Executable loaders (NRO).
* `src/core/system.hpp`: `System` class tying memory, CPU, kernel and loader together.
* `src/core/hle/`: High-Level Emulation (Kernel SVCs; OS services later).
* `src/video_core/`: Vulkan implementation and hardware renderer.
* `docs/`: Hardware and OS documentation — start at [docs/README.md](docs/README.md).
* `externals/`: Third-party dependencies (ballistic, SDL3, etc.).

## Development Roadmap (2026)

## Current Phase:
  * **Phase 2: CPU Emulation (ARM64)** — integer interpreter working, covered by automated tests.

### Phase 1: Core Infrastructure
- [ ] **Logging Framework:** Implementation of a high-performance, thread-safe logging system.
- [ ] **Memory Management Unit:** Development of a VMM with support for 4KB and 64KB page granularity.
- [ ] **Virtual File System (VFS):** Initial support for parsing NCA and HFS2 container formats.
- [x] **NRO Loader:** Loads homebrew `.nro` files (segments, memory map, NACP title). See [docs/07-nexo-internals/nro-loader-and-hle.md](docs/07-nexo-internals/nro-loader-and-hle.md).
- [ ] **Command Line Interface:** Robust argument parsing for debugging and trace orchestration.

### Phase 2: CPU Emulation (ARM64)
- [x] **State Management:** Implementation of the ARM64 register set (X0-X30, SP, PC, and PSTATE).
- [~] **Instruction Decoder:** Integer, scalar FP and the common SIMD/NEON subset of ARMv8.2-A done, checked bit-for-bit against a reference ARM (QEMU). Rarer SIMD forms pending. See [docs/07-nexo-internals/cpu-interpreter.md](docs/07-nexo-internals/cpu-interpreter.md).
- [x] **Execution Loop:** Basic Fetch-Decode-Execute cycle for architectural verification.
- [x] **CPU Tests:** `nexo2_tests` runs hand-written ARM64 programs and clang-compiled C code.
- [~] **JIT Integration:** Ballistic front-end (ARM64 -> IR) wired in; no backend yet.

### Phase 3: Graphics Subsystem (Vulkan)
- [ ] **Vulkan Backend:** Initialization of the Vulkan 1.3 instance and physical device selection.
- [x] **Windowing Integration:** SDL3 window with ImGui; the "Pantalla" window shows what the program presents.
- [x] **Software framebuffer:** `vi` + `nvdrv` (nvmap, binder buffer queue, block-linear deswizzle). libnx console homebrew is displayed. See [docs/07-nexo-internals/display.md](docs/07-nexo-internals/display.md).
- [ ] **VMA Integration:** Implementation of the Vulkan Memory Allocator for emulator-to-GPU mapping.
- [ ] **Shader Pipeline:** Preliminary research into Ampere microcode-to-SPIR-V translation.

### Phase 4: OS Kernel & Services (HLE)
- [~] **Supervisor Call Dispatcher:** HLE kernel with the first SVCs (SetHeapSize, QueryMemory, ExitProcess, OutputDebugString, GetInfo...). Homebrew ABI loader config.
- [~] **Service Infrastructure:** IPC (HIPC/CMIF/TIPC, domains), handle table, events, shared memory. Services: `sm:`, `set:sys`, `apm`, `appletOE`, `hid`, `time`, `fsp-srv` (SD card on a PC folder), `vi`, `nvdrv` (screen). See [docs/07-nexo-internals/ipc-and-services.md](docs/07-nexo-internals/ipc-and-services.md).
- [ ] **Scheduler:** Basic multi-core thread scheduling and synchronization primitives.

## Development Status

NeXo 2 is currently in **Phase 2 (CPU Emulation)**. The project does not currently possess an executable binary capable of loading commercial software. This repository is maintained strictly for educational and research purposes.

## Contribution

Technical contributions regarding C++20, ARM64 assembly, and Vulkan API implementation are welcome via Pull Requests.

## License

This project is distributed for educational purposes and hardware preservation research.