# NeXo 2 — Reference (Switchbrew)

Curated index of the [Switchbrew wiki](https://switchbrew.org/wiki/Main_Page) for
building NeXo 2 (Nintendo Switch 2 / Tegra T239).

Switchbrew documents the Nintendo Switch family. This reference keeps only what
applies to the **Switch 2**: its own hardware pages, plus the **Horizon OS** base
(kernel, services, formats) that the Switch 2 inherits essentially unchanged. The
Switch-1-only hardware (Tegra X1, Maxwell GPU, original cartridge/Joy-Con) is left
out on purpose.

> Links + short summaries for research. The real content lives on switchbrew.org.

## Index

| File | Topic | NeXo roadmap |
| :--- | :--- | :--- |
| [01-hardware.md](01-hardware.md) | Switch 2 SoC / fuses / controllers | Target hardware, video_core |
| [02-horizon-os.md](02-horizon-os.md) | Kernel, SVC, memory layout, crypto (shared OS base) | Phase 2 (CPU), Phase 4 (HLE) |
| [03-services-ipc.md](03-services-ipc.md) | Services API, HIPC (shared OS base) | Phase 4 (HLE services) |
| [04-filesystem-formats.md](04-filesystem-formats.md) | NCA / NSO / NRO containers (shared) | Phase 1 (VFS), loaders |

## Why the OS pages count as "Switch 2"

The Switch 2 runs Horizon OS ("Next"), the **same** microkernel OS as the original
console. The kernel, the SVC ABI, IPC and the executable/container formats carry
over almost entirely, so those switchbrew pages apply to the Switch 2 even though
they were first written for the Switch 1 — that is the "it's the same" case. Where
the Switch 2 diverges (new keys, added services), use the wiki's Switch 2 section.
