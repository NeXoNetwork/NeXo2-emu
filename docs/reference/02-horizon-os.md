# 02 — Horizon OS (Kernel & low level)

Horizon is Nintendo's microkernel OS. This is the heart of HLE and the CPU
side of the emulator. **Highest priority for NeXo's Phase 2 and Phase 4.**

| Page | What it covers | NeXo use |
| :--- | :--- | :--- |
| [Kernel](https://switchbrew.org/wiki/Kernel) | Microkernel design, threads, processes, handles. | HLE kernel model. |
| [SVC](https://switchbrew.org/wiki/SVC) | **Supervisor Calls** — the syscall ABI (SVC numbers, args). | Intercepting `svc` instructions in the CPU. |
| [Secure Monitor](https://switchbrew.org/wiki/Secure_Monitor) | EL3 secure world, SMC calls. | Boot / security context. |
| [Memory layout](https://switchbrew.org/wiki/Memory_layout) | Virtual address space, regions, page sizes. | Your VMM (page granularity, mappings). |
| [Cryptosystem](https://switchbrew.org/wiki/Cryptosystem) | Keys, AES usage, keyslots. | Decrypting containers (later). |
| [Error codes](https://switchbrew.org/wiki/Error_codes) | Result codes / exception model. | Correct HLE return values. |
