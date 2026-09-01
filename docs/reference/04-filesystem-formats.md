# 04 — Filesystem & Container Formats

How software is packaged and stored. Needed to load a title into the emulator.
**NeXo Phase 1 (VFS) and the executable loader.**

## Storage
| Page | What it covers |
| :--- | :--- |
| [SD Filesystem](https://switchbrew.org/wiki/SD_Filesystem) | Layout of the SD card. |
| [Flash Filesystem](https://switchbrew.org/wiki/Flash_Filesystem) | Internal NAND layout. |

## Container / executable formats
| Page | What it covers | NeXo use |
| :--- | :--- | :--- |
| [NCA](https://switchbrew.org/wiki/NCA) | Nintendo Content Archive — the encrypted content container. | Top-level package parsing. |
| [PFS0 / NSP](https://switchbrew.org/wiki/PFS0) | Partition FS used by NSP packages. | Reading installable titles. |
| [RomFS](https://switchbrew.org/wiki/RomFS) | Read-only asset filesystem inside a title. | Game data access. |
| [NSO](https://switchbrew.org/wiki/NSO) | Main executable format (loaded by the OS). | The binary your CPU/JIT runs. |
| [NRO](https://switchbrew.org/wiki/NRO) | Relocatable homebrew executable. | Easiest first thing to try loading. |
| [NPDM](https://switchbrew.org/wiki/NPDM) | Program metadata / permissions. | Process setup. |
| [NACP](https://switchbrew.org/wiki/NACP) | Application control data (title, version). | Metadata / UI. |

> Tip for NeXo: **NRO homebrew** is the simplest realistic first load target —
> no encryption, self-contained. A good milestone once the CPU can execute.
