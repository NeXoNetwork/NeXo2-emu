# Compatibility Mode (Switch 1 games on Switch 2)

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## Compatibility Mode (Switch 1 games on Switch 2)

Source: <https://switchbrew.org/wiki/Switch_2:_Compatibility_Mode>

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
