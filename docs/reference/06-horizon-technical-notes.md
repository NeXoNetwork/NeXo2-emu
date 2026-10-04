# 06 — Horizon OS technical notes (SVC, memory, IPC, formats, services)

Condensed from Switchbrew. Collected: 2026-10-04. Shared base with Switch 1;
verify against the Switch 2 section when behaviour differs.

## SVC ([source](https://switchbrew.org/wiki/SVC))

Calling convention: AArch64 `svc #imm`; args X0-X7, results W0/X0 (result code)
then X1... Outputs. Handles 0xFFFF8000/0xFFFF8001 are pseudo-handles
(current thread/process). Alignment: pages 0x1000, some ops 2 MB.

| SVC | Name / group |
| :--- | :--- |
| 0x01 | SetHeapSize |
| 0x02 | SetMemoryPermission |
| 0x03 | SetMemoryAttribute |
| 0x04 / 0x05 | MapMemory / UnmapMemory |
| 0x06 | QueryMemory |
| 0x07 | ExitProcess |
| 0x08 / 0x09 | CreateThread / StartThread |
| 0x0A / 0x0B | ExitThread / SleepThread |
| 0x0C-0x0F | Get/SetThreadPriority, Get/SetThreadCoreMask |
| 0x11 / 0x12 | SignalEvent / ClearEvent |
| 0x18 | WaitSynchronization (max 64 handles) |
| 0x1F | ConnectToNamedPort |
| 0x21 | SendSyncRequest |
| 0x29 | GetInfo |
| 0x2C / 0x2D | MapPhysicalMemory / UnmapPhysicalMemory |
| 0x40-0x44 | Sessions, Reply/Receive |
| 0x60-0x6D | Debug |
| 0x79-0x7B | Create/Start/TerminateProcess |

Common result codes (module 1, kernel): 0xCC01 unaligned address,
0xCA01 unaligned size, 0xE401 invalid handle, 0xEA01 timeout, 0xEC01 cancelled.
Full table: use the source page.

## Memory layout ([source](https://switchbrew.org/wiki/Memory_layout))

- Process address space: 32, 36 or 39/38-bit (per NPDM), ASLR with 2 MB-aligned
  regions (ReservedHeap, ReservedMap, NewReservedMap).
- Kernel (v4.0.0 example): .text at 0xFFFFFFF7FFC00000; GIC distributor mapped at
  0xFFFFFFF7FFDFB000 (phys 0x50041000).
- Memory controller carveouts: TZDRAM, VPR, SEC, MTS + 5 general (GSC1-5).
- Hint: for HLE the VMM only needs the process-level view (code, heap, stack,
  TLS, alias, aslr, reserved regions).

## HIPC / CMIF ([source](https://switchbrew.org/wiki/HIPC))

Message lives in the Thread Local Region (TLS, 0x200 bytes, IPC buffer at +0x0).

- Header0 (u32): bits 0-15 type, 16-19 pointer count, 20-23 send count,
  24-27 receive count, 28-31 exchange count.
- Header1 (u32): bits 0-9 raw words, 10-13 receive-list count, 20-30 receive-list
  offset, 31 has special header.
- CMIF `InHeader`: magic `SFCI`, version, method id, token. `OutHeader`: magic
  `SFCO`, result code.
- Command types: 1 Request(InvokeMethod), 2 Close, 3 Control, 4/5 RequestWithContext
  (old), 6/7 RequestWithContext. Control commands: ConvertCurrentObjectToDomain,
  CopyFromCurrentDomain, CloneCurrentObject, QueryPointerBufferSize,
  CloneCurrentObjectEx.
- Domains: multiplex object ids over one session.
- TIPC: tiny protocol used only by `sm:` and `pgl`.
- Buffer attributes: In, Out, MapAlias (0x4 type A/B), Pointer, FixedSize, AutoSelect.

## NSO ([source](https://switchbrew.org/wiki/NSO))

Header magic `NSO0`, 0x100 bytes then segments (.text/.rodata/.data), optionally
LZ4 compressed (Zstd flag on 22.0.0+). Segment headers: file offset, memory
offset, size (3 x u32 at 0x10, 0x20, 0x30); BSS size 0x3C; ModuleId 0x40;
compressed sizes 0x60; hashes 0xA0. Flags: bits 0-2 compressed, 3-5 check hash,
6 execute-only (20.0.0+), 7 Zstd (22.0.0+).

## NRO ([source](https://switchbrew.org/wiki/NRO))

Magic `NRO0` at 0x10 (after a 0x10 start/MOD0 pointer header). Fields: version,
size, flags, text/ro/data (offset+size), bss size, module id (0x20), dso handle,
embedded and dyn str/sym offsets. Optional `ASET` asset section appended
(icon 256x256 JPEG, NACP, RomFS). Recommended first load target.

## Services ([source](https://switchbrew.org/wiki/Services_API))

Core: `sm:` service manager, `fsp-srv` filesystem, `hid` input, `nvdrv` GPU
driver, `vi` display, `appletOE/AE` applets, `set`/`set:sys` settings, `pm:*`
process management, `ldr:*` loader, `lm` logging, `time:*`, `acc:*` accounts,
`audout/audren` audio, `bsd/nifm/ssl` network, `spl:` crypto, `es` tickets,
`nfc/nfp` NFC. Switch 2 adds/changes services; check the Switch 2 title list.
Suggested stub order for a first homebrew: `sm:`, `fsp-srv`, `set`, `time`,
`hid`, `vi`, `nvdrv`, `appletOE`, `lm`.
