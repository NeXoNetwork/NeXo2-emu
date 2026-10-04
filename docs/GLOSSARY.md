# Glossary

Short definitions of the terms used across these docs.

| Term | Meaning |
| :--- | :--- |
| **HLE** | High-Level Emulation: reimplement what an OS function/service *does* instead of running the real code. |
| **LLE** | Low-Level Emulation: emulate the real hardware and run the original code. |
| **Interpreter** | CPU emulator that decodes and executes one guest instruction at a time. Simple, slow. |
| **JIT / dynarec** | Just-In-Time recompiler: translates blocks of guest code (ARM64) into host code (x86-64) and runs them. Fast. |
| **IR** | Intermediate Representation: neutral form a JIT uses between decoding and generating host code. |
| **Guest / Host** | Guest = the emulated console. Host = your PC. |
| **T239 / Drake** | The Switch 2 SoC (NVIDIA). |
| **Ounce** | Internal codename of the Switch 2 platform (`nn::pl::IsRunningOnOunce`). |
| **BEE** | Product-code prefix of Switch 2 hardware (BEE-CPU-01, BEE-CDH...). |
| **Cortex-A78C** | The ARM CPU cores in T239 (ARMv8.2-A + extras). |
| **PAC** | Pointer Authentication: ARM instructions that sign/check pointers (`paciasp`, `autiasp`, `retaa`). |
| **Ampere / GA10B** | NVIDIA GPU architecture/family used by T239. |
| **NVN / NVN2** | Nintendo's proprietary graphics API (Switch 1 / Switch 2). |
| **Vulkan** | Open graphics API that NeXo uses on the host to draw. |
| **Horizon** | Nintendo's microkernel OS (Switch 1 and 2). |
| **Kernel** | Core of the OS: threads, memory, handles, IPC. |
| **SVC** | Supervisor Call: a syscall; the `svc #n` instruction jumps from app to kernel. |
| **Handle** | Integer that refers to a kernel object (thread, event, session...). |
| **VMM / MMU** | Virtual memory manager / memory management unit: maps virtual to physical addresses in pages. |
| **Page** | Unit of memory mapping (4 KB, 64 KB...). |
| **TLS** | Thread Local Storage; on Horizon also holds the IPC message buffer. |
| **IPC / HIPC** | Inter-Process Communication; HIPC is Horizon's IPC message format. |
| **CMIF / TIPC** | The two protocols on top of HIPC (full / tiny). |
| **Service** | A system process that apps talk to via IPC (`fsp-srv`, `hid`, `vi`...). |
| **sm:** | Service Manager: gives an app a handle to a named service. |
| **Domain** | Several service objects multiplexed over one IPC session. |
| **NCA** | Nintendo Content Archive: encrypted container for titles. |
| **NSP / PFS0** | Package / partition filesystem holding NCAs. |
| **RomFS** | Read-only filesystem with game assets. |
| **NSO** | Main executable format of system/games. |
| **NRO** | Homebrew executable format (easiest to load first). |
| **NPDM** | Program metadata: permissions, allowed SVCs, core mask. |
| **NACP** | Application control data: name, version, icon info. |
| **Fuses** | One-time programmable bits in the SoC (security config, keys). |
| **UFS / LUN** | Flash storage type / logical partition inside it. |
| **Tick frequency** | Rate of the system timer: 19.2 MHz on Switch 1, 31.25 MHz on Switch 2. |
