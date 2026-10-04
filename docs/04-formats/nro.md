# NRO homebrew format

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## NRO

Source: <https://switchbrew.org/wiki/NRO>

Magic `NRO0` at 0x10 (after a 0x10 start/MOD0 pointer header). Fields: version,
size, flags, text/ro/data (offset+size), bss size, module id (0x20), dso handle,
embedded and dyn str/sym offsets. Optional `ASET` asset section appended
(icon 256x256 JPEG, NACP, RomFS). Recommended first load target.
