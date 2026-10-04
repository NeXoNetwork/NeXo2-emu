# HIPC / CMIF message format

Condensed from the Switchbrew wiki. Collected 2026-10-04 — re-check the source pages, they change often.


## HIPC / CMIF

Source: <https://switchbrew.org/wiki/HIPC>

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
