# 07 — External resources (links)

Collected: 2026-10-04.

## CPU (Cortex-A78C / ARMv8.2-ARMv9)
- [Cortex-A78C r0p2 Technical Reference Manual](https://documentation-service.arm.com/static/6193c9bef45f0b1fbf3a85dd)
- [Cortex-A78 r1p1 TRM](https://documentation-service.arm.com/static/5f159b6720b7cf4bc5247448)
- [Cortex-A78 Cryptographic Extension](https://documentation-service.arm.com/static/5f16a70620b7cf4bc52495f3)
- [Cortex-A78C product page](https://developer.arm.com/Processors/Cortex-A78c)
- Arm Architecture Reference Manual for A-profile (DDI 0487) — instruction encodings, system registers, PAC/MTE: search "DDI0487" on developer.arm.com.
- Arm A64 ISA XML (machine-readable encodings, useful for decoder generation): developer.arm.com -> "A64 Instruction Set Architecture".

## JIT / recompilers
- [pound-emu/ballistic](https://github.com/pound-emu/ballistic) — ARM64 recompiler (rewrite of dynarmic), CMake + LuaJIT, clang/clang-cl. Already a submodule in `externals/ballistic`.

## NVIDIA T234 (Orin) as the closest public relative of T239
- [Jetson Linux Developer Guide (Orin)](https://docs.nvidia.com/jetson/archives/r36.5.2/DeveloperGuide/HR/JetsonModuleAdaptationAndBringUp/JetsonOrinNxNanoSeries.html)
- [LineageOS android_device_nvidia_t234-common](https://github.com/LineageOS/android_device_nvidia_t234-common) — device tree/drivers for T234.
- [Methodically Defeating Nintendo Switch Security (arXiv)](https://ar5iv.labs.arxiv.org/html/1905.07643) — Switch 1 security architecture, still useful background.
- NVIDIA open GPU kernel modules / open-gpu-doc (GitHub: NVIDIA/open-gpu-kernel-modules, NVIDIA/open-gpu-doc) — Ampere class headers.
- Mesa NVK / nouveau (Vulkan driver for NVIDIA, Ampere support) — reference for GA10x command streams.

## Graphics
- [Vulkan 1.3 specification](https://registry.khronos.org/vulkan/specs/1.3/html/)
- NVN is proprietary; public information is limited to what the Switch 1 community documented (Switchbrew `NVN`/`NV services` pages). NVN2 has no public documentation.

## Community
- [Switchbrew wiki](https://switchbrew.org/wiki/Main_Page) — main source.
- [Switch 2 news/archive](https://switchbrew.org/wiki/Switch_2:_News/Archive)
- [Mirage (Rust firmware reimplementation)](https://github.com/mirage-rs/Mirage) — HLE structure reference for Switch 1.
- Other open emulators for architecture ideas: yuzu forks (Eden, Sudachi archives), Ryujinx forks — study HLE service layout only; respect licences.

## Notes
- Anything beyond hardware/OS documentation (keys, firmware dumps) must come from the user's own console; do not store them in the repo.
