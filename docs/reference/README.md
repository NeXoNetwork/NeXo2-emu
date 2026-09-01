# NeXo 2 — Reference (Switchbrew)

Organized index of the [Switchbrew wiki](https://switchbrew.org/wiki/Main_Page),
the reverse-engineering documentation for the Nintendo Switch, curated for
building NeXo 2. Most of the wiki documents the original Switch (Horizon OS,
which the Switch 2 inherits and extends); a growing **Switch 2** section covers
the T239-specific parts.

> These are links and short summaries for research. The actual content lives on
> switchbrew.org — always check the wiki for the authoritative, up-to-date info.

## Index

| File | Topic | Maps to NeXo roadmap |
| :--- | :--- | :--- |
| [01-hardware.md](01-hardware.md) | SoC, CPU, GPU, fuses, cartridge (Switch 1 & 2) | Target hardware, video_core |
| [02-horizon-os.md](02-horizon-os.md) | Kernel, SVC, memory layout, crypto | Phase 2 (CPU), Phase 4 (HLE kernel) |
| [03-services-ipc.md](03-services-ipc.md) | Services API, HIPC/IPC | Phase 4 (HLE services) |
| [04-filesystem-formats.md](04-filesystem-formats.md) | FS, NCA/NSO/NRO containers | Phase 1 (VFS), loaders |
| [05-software-network.md](05-software-network.md) | System versions, titles, network | Context / later phases |

## How to use this while building NeXo

- Writing the **ARM64 decoder/JIT**? Start with the CPU + SVC pages (02).
- Building the **memory manager (VMM)**? See Memory layout (02) + Tegra pages (01).
- Wiring **HLE services**? Services API + HIPC (03) are the core.
- Loading a **binary**? NSO/NRO/NCA container formats (04).
