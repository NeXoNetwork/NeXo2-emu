# 01 — Hardware (Switch 2)

The Switch 2 physical platform: the T239 SoC and its parts. This is what NeXo's
target-hardware model and the future `video_core` aim at.

| Page | What it covers |
| :--- | :--- |
| [Switch 2: Tegra T239](https://switchbrew.org/wiki/Switch_2:_Tegra_T239) | The custom Nvidia SoC (ARM Cortex-A78C + Ampere GPU) NeXo targets. |
| [Switch 2: Fuses](https://switchbrew.org/wiki/Switch_2:_Fuses) | Hardware fuse configuration / security. |
| [Joy-Con 2](https://switchbrew.org/wiki/Joy-Con_2) | The new controllers (input emulation, later). |
| [Switch 2: Product Information](https://switchbrew.org/wiki/Switch_2:_Product_Information) | Model / serial data. |

## Notes in this folder

| File | Content |
| :--- | :--- |
| [target-specs.md](target-specs.md) | Specs NeXo uses as its baseline |
| [t239-soc.md](t239-soc.md) | T239: CPU, caches, GPU, RAM, PAC, memory encryption |
| [board-and-dock.md](board-and-dock.md) | Board components and dock chips |
| [fuses.md](fuses.md) | Fuse block layout and MMIO mirrors |
| [flash-filesystem.md](flash-filesystem.md) | UFS LUN layout (boot chain) |
| [joy-con-2.md](joy-con-2.md) | Joy-Con 2 hardware and safe mode |
