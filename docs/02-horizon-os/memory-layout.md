# Memory layout

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## Memory layout

Source: <https://switchbrew.org/wiki/Memory_layout>

- Process address space: 32, 36 or 39/38-bit (per NPDM), ASLR with 2 MB-aligned
  regions (ReservedHeap, ReservedMap, NewReservedMap).
- Kernel (v4.0.0 example): .text at 0xFFFFFFF7FFC00000; GIC distributor mapped at
  0xFFFFFFF7FFDFB000 (phys 0x50041000).
- Memory controller carveouts: TZDRAM, VPR, SEC, MTS + 5 general (GSC1-5).
- Hint: for HLE the VMM only needs the process-level view (code, heap, stack,
  TLS, alias, aslr, reserved regions).
