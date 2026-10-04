# T239 SoC (Switch 2)

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## T239 SoC

Source: <https://switchbrew.org/wiki/Switch_2:_Tegra_T239>

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
