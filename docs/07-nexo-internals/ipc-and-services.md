# IPC and HLE services

How a program talks to system services in NeXo 2, and how to add a new service.

## The path of a call

```
program:  svcConnectToNamedPort("sm:")          -> handle to sm:
          [message in TLS] svcSendSyncRequest(handle)
kernel:   Kernel::ProcessIpcRequest(handle, TLS)
            ParseHipcRequest()      header, special header (PID, handles), X/A/B/W/C descriptors
            CMIF?  SFCI header -> command id  (domain? -> pick the object by id)
            TIPC?  type = 16 + command id
            ServiceObject::Dispatch(ctx) -> the C++ handler
            IpcContext::WriteResponse()  SFCO + result + data (+ handles / object ids)
program:  reads the response from TLS
```

| File | Role |
| :--- | :--- |
| `src/core/hle/kernel_objects.hpp` | `HandleTable`, `KClientSession`, `SessionState` (domains) |
| `src/core/hle/ipc.hpp/.cpp` | Message parsing (`ParseHipcRequest`) and `IpcContext` (args in, response out) |
| `src/core/hle/service.hpp/.cpp` | `ServiceObject` (command table), `UnimplementedService`, `ServiceRegistry` |
| `src/core/hle/services/sm.*` | `sm:` - RegisterClient, GetServiceHandle, DetachClient (CMIF and TIPC) |
| `src/core/hle/services/set.*` | `set:sys` - GetFirmwareVersion / GetFirmwareVersion2 |
| `src/core/hle/services/apm.*` | `apm` - OpenSession, performance mode/configuration |
| `src/core/hle/services/applet.*` | `appletOE` -> IApplicationProxy and its sub-interfaces (state, self, window, functions...) |
| `src/core/hle/services/hid.*` | `hid` - CreateAppletResource, shared memory (0x40000) with two connected controllers fed from the PC keyboard/gamepad (see [input.md](input.md)), Activate*/SetSupported* stubs |
| `src/core/hle/services/time.*` | `time:u/a/s` - clocks, time zone (UTC), shared memory filled with the PC's clock |
| `src/core/hle/services/fs.*` | `fsp-srv` - SD card backed by a PC folder: IFileSystem (create/delete/rename/open files and folders, entry type, free space), IFile (read/write/size), IDirectory (list) |
| `src/core/hle/services/vi.*`, `nvdrv.*` | Display: `vi:m/s/u` and `nvdrv` - see [display.md](display.md) |
| `src/core/hle/firmware.hpp` | Firmware version NeXo reports (20.1.0) |

Message format reference: [../03-services-ipc/hipc.md](../03-services-ipc/hipc.md).
NeXo follows the same layout as libnx (`nx/include/switch/sf/hipc.h`, `cmif.h`, `tipc.h`).

## What is supported

- **SVCs:** `ConnectToNamedPort` (only "sm:"), `SendSyncRequest`, `SendSyncRequestWithUserBuffer`, `CloseHandle`,
  `ClearEvent`/`ResetSignal`, `WaitSynchronization` (no threads: an endless wait halts the CPU),
  `MapSharedMemory`/`UnmapSharedMemory` (the content is copied when mapping), `CreateTransferMemory`
  (handle only, the memory stays in place), and single-thread versions of `ArbitrateLock/Unlock`,
  `WaitProcessWideKeyAtomic` (returns timed out) and `SignalProcessWideKey`.
- **CMIF:** Request, Close, and the Control commands ConvertCurrentObjectToDomain,
  CopyFromCurrentDomain, CloneCurrentObject(Ex), QueryPointerBufferSize (0x8000).
- **Domains:** one session holding several objects; domain Close.
- **TIPC:** for services that enable it (`sm:`).
- **Buffers:** A/B/W/X/C descriptors are parsed. `ctx.ReadBuffer(i)` takes A (or X if A is empty),
  `ctx.WriteBuffer(i)` writes to B (or C if B is empty).
- **Out interfaces:** `ctx.PushInterface(obj)` returns a new session handle, or an object id in a domain.

## Missing services stop the CPU with a clear message

If a program asks `sm:` for a service NeXo does not have, it still gets a handle
(to an `UnimplementedService`). The first command it sends stops the CPU with:

```
Servicio 'fsp-srv': comando 1 no implementado
```

The same happens with an unknown command of an existing service. That message,
in the console and in `nexo2.log`, is the to-do list.

## Adding a service

1. Create `src/core/hle/services/<name>.hpp/.cpp` with a class deriving from `ServiceObject`.
2. In the constructor, register each command:
   `RegisterCommand(3, "GetFirmwareVersion", [this](IpcContext& ctx) { GetFirmwareVersion(ctx); });`
3. In the handler: read args with `ctx.Pop<T>()`, buffers with `ctx.ReadBuffer()`,
   answer with `ctx.Push<T>()`, `ctx.WriteBuffer()`, `ctx.PushInterface()`, and `ctx.SetResult()`.
4. Register it by name in `Kernel::RegisterDefaultServices()` (kernel.cpp).
5. Add the .cpp to `nexo2_core` in `CMakeLists.txt`.
6. Test it from a homebrew in `tests/programs/nro_<name>/` (see `nro_ipc/main.c` and
   `nro_common/nx_min.h`), regenerate with `python tools/make_nro.py`, add a TEST.

## libnx start-up: covered

libnx's default start-up (`__appInit`, `nx/source/runtime/init.c`) is covered end to end:
`sm:` -> `apm` -> `appletOE` (+ IApplicationProxy and 7 sub-interfaces) -> `hid`
(+ shared memory) -> `time:u` (+ shared memory, calendar) -> `fsp-srv` (domain, cloned
sessions, SD card IFileSystem). `set:sys` is skipped thanks to the HosVersion loader entry.

`tests/programs/nro_libnx_init/main.c` replays those 45 calls with the same commands and
formats as libnx; `tests/services_tests.cpp` checks them.

After start-up, the SD card (`fsp-srv` IFileSystem) and the screen (`vi:m`, `nvdrv`, see
[display.md](display.md)) are implemented too, so a libnx console homebrew runs and draws.
Controllers work too ([input.md](input.md)). Still missing: threads, audio... Each one
shows up as a "no implementado" message.

## SD card (`sdmc:/`)

The SD card is a normal folder on the PC: `sdmc` next to the executable (the UI shows the full
path). `Kernel::SetSdmcRoot()` changes it. Switch paths ("/config/app.ini") are split on '/'
and joined to that folder; any `..`, `\` or `:` part is rejected, so a program can never reach
files outside the folder (`ResolveGuestPath`, tested in `tests/display_tests.cpp`).
