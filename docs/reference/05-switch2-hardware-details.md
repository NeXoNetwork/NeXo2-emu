# 05 — Switch 2 hardware details (collected notes)

Condensed from the Switchbrew wiki (Switch 2 section). Always re-check the source
page: the Switch 2 pages change often. Collected: 2026-10-04.

## T239 SoC ([source](https://switchbrew.org/wiki/Switch_2:_Tegra_T239))

Custom SoC designed to Nintendo's specifications, similar to NVIDIA's T234 (Orin).

| Item | Detail |
| :--- | :--- |
| CPU | 8x 64-bit ARM Cortex-A78C |
| L1 | 64 KB I + 64 KB D per core |
| L2 | 256 KB per core |
| L3 | 4 MB shared |
| GPU | Ampere, 1536 cores |
| RAM | 12 GB LPDDR5X (2x 6 GB), 128-bit bus, ~102 GB/s |
| Security | Pointer Authentication (Armv8.3 PAC + Armv8.6 EPAC) |
| Memory encryption | Selective, in carveout regions only; application memory is NOT encrypted. Per-physical-address tweak, no authentication (tampering gives garbled plaintext). |

Emulation relevance: PAC means guest binaries may contain PAC instructions
(`paciasp`, `autiasp`, `pacia`, `retaa`...). The CPU decoder must at least
treat them correctly (NOP-like or real signing) — see compat-mode below, where
all `nnCompat*` modules are built with PAC.

## Board and components ([source](https://switchbrew.org/wiki/Switch_2:_Hardware))

| Part | Detail |
| :--- | :--- |
| Board code | BEE-CPU-01 (retail) |
| SoC | NVIDIA GMLX30-A1 (T239) |
| Storage | 256 GB UFS 3.1 |
| RAM | 12 GB LPDDR5X, BGA-441 (JEDEC MO-342), JESD209-5C ballout |
| Wireless | MediaTek MT3681AEN (MT7961) |
| PMIC | Maxim MAX77851 (main), Renesas DA9092 (secondary) |
| Audio codec | Realtek ALC5658 |
| Voice | Intelligo IG2200 |
| USB hub | Genesys GL852G |
| USB-C | Cypress CYPD6228 |
| Charger / gauge | MAX77986 / MAX17050 |
| Sensors | Rohm BH1730FVC (light), TI TMP451 (temp) |

## Dock BEE-CDH-MAIN-01 ([source](https://switchbrew.org/wiki/Switch_2:_Dock))

Realtek RTD2175N (DP to HDMI), Genesys GL3510 hub, ST STM32G0B0 MCU, Macronix
MX25V80066 flash, Realtek RTL8153B (LAN), 2x Cypress PD controllers.

## Fuses ([source](https://switchbrew.org/wiki/Switch_2:_Fuses))

- T23x fuse block: 0x1000 bytes, 32-bit words. Layout reverse-engineered from
  Linux4Tegra `fskp_t234.bin`.
- Shadowed fuses: words 0-79 = 40 redundant pairs (OR'd together).
- H2 fuses: words 188-259, CRC32 protected (poly 0x1EDC6F41) — public key hashes,
  keys, SW ODM data.
- MMIO mirrors: ProductionMode 0x3810100, OdmLock 0x3810108, SecurityMode
  0x38101A0, ArmJtagDisable 0x38101B8, DebugAuthentication 0x38102E4.
- Key bits (word 0): OdmLock 5-8, Fa 9, SecurityMode 10, ArmJtagDisable 11,
  NvJtagProtectionEnable 12, DebugAuthentication 16-20.
- Emulator use: only to emulate fuse MMIO reads if a boot-level (LLE) path is
  ever attempted. Not needed for HLE.

## Flash filesystem ([source](https://switchbrew.org/wiki/Switch_2:_Flash_Filesystem))

256 GB UFS, 4 LUNs, signed and encrypted. LUN0 layout:

| Offset | Size | Component |
| :--- | :--- | :--- |
| 0x0 | 0x2000 | BRBCT |
| 0x1E000 | 0x2000 | MB1 BCH |
| 0x20000 | var | MB1 |
| 0x9E000 | 0x2000 | PSC-BL BCH |
| 0xA0000 | var | PSC-BL |
| 0xC0000 | 0x2000 | MB1-BCT BCH |
| 0xC2000 | var | MB1-BCT |
| 0x103000 | 0x1000 | BootConfig |
| 0x104000 | var | Package2 |

LUN1 is a backup copy of LUN0. LUN2/LUN3 not fully documented.

## Joy-Con 2 ([source](https://switchbrew.org/wiki/Joy-Con_2))

- SoC MediaTek MT3689BCA (both), IMU TDK ICM-42670-P, magnetometer AKM AK09919C,
  PMIC TI BQ25618, amp ADI MAX98388. Right has NXP PN71602 NFC.
- Safe mode / DFU: hold Plus/Minus + ZR/ZL + Sync, release ZR/ZL, then
  Plus/Minus, then Sync. LEDs 1 and 4 light up; enumerates as "Nintendo Safe
  Mode Device" over the charging grip.
- Wireless protocol details: see the source page (not summarized here).

## System versions ([source](https://switchbrew.org/wiki/Switch_2:_System_Versions))

Launch firmware 19.0.0/19.1.0; 20.1.1 (2025-06-04), 20.5.0 (2025-09-30),
21.0.0 (2025-11-11), 22.0.0 (2026-03-17), 22.5.0 (2026-06-16),
23.0.0 (2026-09-10). Per-version pages exist: `Switch_2:_22.1.0`, `22.5.0`,
`23.0.0`, `23.0.1`. System title list:
[Switch_2:_Title_list](https://switchbrew.org/wiki/Switch_2:_Title_list)
(USB, boot2, settings, bus, btdrv, bcat, friends, nifm, ptm, socket, HID, audio, NFC, SSL...).

## Compatibility Mode (Switch 1 games on Switch 2) ([source](https://switchbrew.org/wiki/Switch_2:_Compatibility_Mode))

Three PAC-enabled modules are injected between the main module and the SDK:

| Module | Role |
| :--- | :--- |
| `nnCompatTrampoline` | Per-game patches |
| `nnCompatThin` | Core API hooks |
| `nnCompat` | Graphics / system translation layer |

- Works by hooking via dynamic linking (symbol overrides).
- NPDM changes: SVC 0x54 and 0x80 allowed; CoreMask 0x3F (vs 0x7).
- `nn::os::GetThreadAvailableCoreMask` returns 0x7 to hide extra cores.
- **System tick frequency changes: 19,200,000 Hz (Switch 1) -> 31,250,000 Hz (Switch 2).**
  Important for the emulated timer (`cntfrq_el0` / `cntpct_el0`).
- `nn::pl::IsRunningOnOunce` returns 1 (Ounce = Switch 2 platform codename).
- `nn::vi::*` replaced (vi:u removed); OpenGL/Vulkan translated to the new GPU.
- `CompatibilityParameter`: 11 ModuleSdkInfo entries at 0x00, count at 0xB0,
  Graphics/Movie/Audio/Misc control data pointers at 0xB8-0xD0.
- `MiscControlData` flag bits: 0 block shader cache access, 1 block shader cache
  type queries, 2 block shader dir open, 4 serialize savedata with mutex,
  6 drop negative vibration, 10 wait for launch logo, 13 sleep for offline web applet.
- Per-title settings come from `ApplicationCompatibilityInfo` JSON
  (application id, target max version, graphics_config, misc_config).
