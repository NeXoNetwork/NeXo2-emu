# Board, components and dock

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## Board and components

Source: <https://switchbrew.org/wiki/Switch_2:_Hardware>

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

## Dock BEE-CDH-MAIN-01

Source: <https://switchbrew.org/wiki/Switch_2:_Dock>

Realtek RTD2175N (DP to HDMI), Genesys GL3510 hub, ST STM32G0B0 MCU, Macronix
MX25V80066 flash, Realtek RTL8153B (LAN), 2x Cypress PD controllers.
