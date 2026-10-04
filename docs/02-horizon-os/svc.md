# Supervisor Calls (SVC)

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## SVC

Source: <https://switchbrew.org/wiki/SVC>

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
