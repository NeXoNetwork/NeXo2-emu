# 03 — Services & IPC

Apps talk to the OS through services over an IPC mechanism — the same design on
Switch 2 as on the original console. Emulating these (HLE) is what lets software
run. **NeXo Phase 4.**

| Page | What it covers | NeXo use |
| :--- | :--- | :--- |
| [Services API](https://switchbrew.org/wiki/Services_API) | Master list of system services (fs, hid, nvdrv, vi, am, ...). | Which services to implement and in what order. |
| [HIPC](https://switchbrew.org/wiki/HIPC) | Horizon IPC: message format, domains, handles. | The transport every service call rides on. |

Suggested first services to stub for a booting app: `sm` (service manager),
`fsp-srv` (filesystem), `hid` (input), `vi`/`nvdrv` (display/GPU), `settings`,
`time`, `pm`. Check the Services API page for the current list. Note the Switch 2
may add new services on top of this shared base.

## Notes in this folder

| File | Content |
| :--- | :--- |
| [hipc.md](hipc.md) | IPC message layout, CMIF, domains, TIPC |
| [services.md](services.md) | Main services and stub order |
