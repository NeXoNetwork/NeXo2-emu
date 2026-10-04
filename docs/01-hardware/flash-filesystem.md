# Flash filesystem (UFS)

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## Flash filesystem

Source: <https://switchbrew.org/wiki/Switch_2:_Flash_Filesystem>

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
