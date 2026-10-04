# 04 — Filesystem & Container Formats

How software is packaged. Needed to load a title into the emulator. The Switch 2
reuses the same format family as the original console (with possible new keys and
minor changes). **NeXo Phase 1 (VFS) and the executable loader.**

| Page | What it covers | NeXo use |
| :--- | :--- | :--- |
| [NCA](https://switchbrew.org/wiki/NCA) | Nintendo Content Archive — the encrypted content container. | Top-level package parsing. |
| [PFS0 / NSP](https://switchbrew.org/wiki/PFS0) | Partition FS used by NSP packages. | Reading installable titles. |
| [RomFS](https://switchbrew.org/wiki/RomFS) | Read-only asset filesystem inside a title. | Game data access. |
| [NSO](https://switchbrew.org/wiki/NSO) | Main executable format loaded by the OS. | The binary your CPU/JIT runs. |
| [NRO](https://switchbrew.org/wiki/NRO) | Relocatable homebrew executable. | Easiest first thing to load. |
| [NPDM](https://switchbrew.org/wiki/NPDM) | Program metadata / permissions. | Process setup. |
| [NACP](https://switchbrew.org/wiki/NACP) | Application control data (title, version). | Metadata / UI. |

> Tip for NeXo: an **NRO homebrew** is the simplest realistic first load target —
> open format, no encryption, self-contained — once the CPU can execute.

## Notes in this folder

| File | Content |
| :--- | :--- |
| [nso.md](nso.md) | NSO header and flags |
| [nro.md](nro.md) | NRO header and ASET assets |
