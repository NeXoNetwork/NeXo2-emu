# Target Hardware Specifications

This document details the estimated technical specifications for the Nintendo
Switch 2 platform (Nvidia T239 SoC), which serve as the baseline for the
development of the NeXo 2 lab.

## 1. Central Processing Unit (CPU)
* **Architecture:** 8x ARM Cortex-A78C cores (customized).
* **ISA:** ARMv8.2-A / ARMv9 (64-bit only). No native AArch32 support.
* **Clock:**
    * Docked mode: ~1.7 GHz (peak).
    * Handheld mode: ~1.1 GHz.
* **Cache:** 4 MB shared L3 / 256 KB L2 per core.

## 2. Graphics Processing Unit (GPU)
* **Architecture:** Nvidia Ampere (custom RTX 30 series).
* **Configuration:** 12 Streaming Multiprocessors (SM).
* **CUDA Cores:** 1,536 cores.
* **Tensor Cores:** 48 (native DLSS 3.1/3.5 support).
* **RT Cores:** 12 (ray tracing support).
* **Theoretical compute:** 0.6 - 4.0 TFLOPS (depending on the power profile).

## 3. Memory (RAM)
* **Type:** 12 GB LPDDR5X.
* **Memory Bus:** 128-bit.
* **Transfer Rate:** 7500 MT/s. *(Check: 102 GB/s on a 128-bit bus is ~6400 MT/s, which matches Switchbrew's LPDDR5X-3200. See [t239-soc.md](t239-soc.md).)*
* **Total Bandwidth:** 102 GB/s.
* **Suggested Split:**
    * 9 GB available for applications.
    * 3 GB reserved for Horizon OS (Next).

## 4. Storage and I/O
* **Internal:** 256 GB UFS 3.1 (read speed ~2100 MB/s).
* **External:** MicroSD Express slot (SD 7.1 support).
* **Video Output:** HDMI 2.1 with 4K HDR and VRR support.

## 5. Development Notes (Emulation)
* **JIT priority:** Implement decoders specific to the Cortex-A78C instruction set.
* **Vulkan priority:** Map Ampere extensions (such as Conservative Rasterization
  or Variable Rate Shading) directly through Vulkan 1.3.
* **Audio:** ARM-based DSP for spatial audio processing.
