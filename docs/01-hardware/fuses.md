# Fuses

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## Fuses

Source: <https://switchbrew.org/wiki/Switch_2:_Fuses>

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
