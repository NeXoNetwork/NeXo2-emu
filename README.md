# NeXo 2 | Emulator for Nintendo Switch 2

NeXo 2 is an open-source research project focused on the study and analysis of the Nintendo Switch 2 (T239) hardware architecture. The project aims to document system components and experiment with low-level compatibility layers using C++20.

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
* `src/core/memory/`: Virtual Memory Manager (VMM).
* `src/core/hle/`: High-Level Emulation (Kernel and OS Services).
* `src/video_core/`: Vulkan implementation and hardware renderer.
* `externals/`: Third-party dependencies (Dynarmic, SDL3, etc.).

## Development Roadmap (2026)

## Current Fhase:
  * **Phase 0: Research**

### Phase 1: Core Infrastructure
- [ ] **Logging Framework:** Implementation of a high-performance, thread-safe logging system.
- [ ] **Memory Management Unit:** Development of a VMM with support for 4KB and 64KB page granularity.
- [ ] **Virtual File System (VFS):** Initial support for parsing NCA and HFS2 container formats.
- [ ] **Command Line Interface:** Robust argument parsing for debugging and trace orchestration.

### Phase 2: CPU Emulation (ARM64)
- [ ] **State Management:** Implementation of the ARM64 register set (X0-X30, SP, PC, and PSTATE).
- [ ] **Instruction Decoder:** Development of a disassembler for the ARMv8.2-A instruction set.
- [ ] **Execution Loop:** Basic Fetch-Decode-Execute cycle for architectural verification.
- [ ] **JIT Integration:** Design of the execution engine backend for Dynarmic integration.

### Phase 3: Graphics Subsystem (Vulkan)
- [ ] **Vulkan Backend:** Initialization of the Vulkan 1.3 instance and physical device selection.
- [ ] **Windowing Integration:** Deployment of the rendering context via SDL3.
- [ ] **VMA Integration:** Implementation of the Vulkan Memory Allocator for emulator-to-GPU mapping.
- [ ] **Shader Pipeline:** Preliminary research into Ampere microcode-to-SPIR-V translation.

### Phase 4: OS Kernel & Services (HLE)
- [ ] **Supervisor Call Dispatcher:** Interrupt handling system for HLE syscall interception.
- [ ] **Service Infrastructure:** Registration and IPC handling for system services (FS, HID, etc.).
- [ ] **Scheduler:** Basic multi-core thread scheduling and synchronization primitives.

## Development Status

NeXo 2 is currently in **Phase 0 (Architectural Exploration)**. The project does not currently possess an executable binary capable of loading commercial software. This repository is maintained strictly for educational and research purposes.

## Contribution

Technical contributions regarding C++20, ARM64 assembly, and Vulkan API implementation are welcome via Pull Requests.

## License

This project is distributed for educational purposes and hardware preservation research.