# NeXo 2 — Documentation

Everything NeXo 2 needs to know about the Nintendo Switch 2 (T239) and its OS,
organised by topic. Main external source: the [Switchbrew wiki](https://switchbrew.org/wiki/Main_Page).

New here? Start with the [GLOSSARY](GLOSSARY.md), then [target-specs](01-hardware/target-specs.md).
Build instructions are in [../BUILDING.md](../BUILDING.md).

## Map

| Folder | Topic | NeXo roadmap |
| :--- | :--- | :--- |
| [01-hardware](01-hardware/README.md) | T239 SoC, board, fuses, flash, Joy-Con 2 | Target hardware, `video_core` |
| [02-horizon-os](02-horizon-os/README.md) | Kernel, SVC, memory layout | Phase 2 (CPU), Phase 4 (HLE) |
| [03-services-ipc](03-services-ipc/README.md) | HIPC/CMIF, system services | Phase 4 (HLE services) |
| [04-formats](04-formats/README.md) | NCA, NSO, NRO, NPDM... | Phase 1 (VFS), loaders |
| [05-switch2-system](05-switch2-system/README.md) | Firmware versions, compatibility mode | Timers, Switch 1 titles |
| [06-resources](06-resources/README.md) | ARM / NVIDIA / Vulkan manuals, tools | All |
| [07-nexo-internals](07-nexo-internals/README.md) | How NeXo's own code works (CPU interpreter, tests) | Phase 2 |

Each folder has a `README.md` with Switchbrew links plus local notes.

## Suggested reading order (by task)

1. **CPU** (`src/core/arm64`): 07-nexo-internals/cpu-interpreter → 01-hardware/t239-soc → 06-resources (ARM manuals) → 02-horizon-os/svc
2. **Memory** (`src/core/memory`): 02-horizon-os/memory-layout
3. **Loading a program**: 04-formats/nro → 04-formats/nso
4. **HLE**: 03-services-ipc/hipc → 03-services-ipc/services
5. **Timers / compatibility**: 05-switch2-system/compatibility-mode

## Why the OS pages count as "Switch 2"

The Switch 2 runs Horizon OS, the **same** microkernel OS as the original console.
Kernel, SVC ABI, IPC and container formats carry over almost unchanged, so those
Switchbrew pages apply. Where the Switch 2 differs (new keys, new services, tick
rate, PAC), the notes in 01 and 05 point it out. Switch-1-only hardware
(Tegra X1, Maxwell, old Joy-Con) is left out on purpose.

> Keys and firmware must come from your own console and never go into this repo.
