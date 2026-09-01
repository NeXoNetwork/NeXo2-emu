# 03 — Services & IPC

Apps talk to the OS through services over an IPC mechanism. Emulating these
(HLE) is what lets software actually run. **NeXo Phase 4.**

| Page | What it covers | NeXo use |
| :--- | :--- | :--- |
| [Services API](https://switchbrew.org/wiki/Services_API) | Master list of system services (fs, hid, nvdrv, vi, am, ...). | Which services to implement and in what order. |
| [HIPC](https://switchbrew.org/wiki/HIPC) | Horizon IPC: message format, domains, handles. | The transport every service call rides on. |

Suggested first services to stub for a booting app: `fsp-srv` (filesystem),
`hid` (input), `vi`/`nvdrv` (display/GPU), `set`/`settings`, `time`, `pm`/`sm`
(service manager). Check the Services API page for the current list.
