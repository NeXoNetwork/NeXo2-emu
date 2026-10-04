# NSO executable format

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## NSO

Source: <https://switchbrew.org/wiki/NSO>

Header magic `NSO0`, 0x100 bytes then segments (.text/.rodata/.data), optionally
LZ4 compressed (Zstd flag on 22.0.0+). Segment headers: file offset, memory
offset, size (3 x u32 at 0x10, 0x20, 0x30); BSS size 0x3C; ModuleId 0x40;
compressed sizes 0x60; hashes 0xA0. Flags: bits 0-2 compressed, 3-5 check hash,
6 execute-only (20.0.0+), 7 Zstd (22.0.0+).
