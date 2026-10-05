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
| `src/core/hle/firmware.hpp` | Firmware version NeXo reports (20.1.0) |

Message format reference: [../03-services-ipc/hipc.md](../03-services-ipc/hipc.md).
NeXo follows the same layout as libnx (`nx/include/switch/sf/hipc.h`, `cmif.h`, `tipc.h`).

## What is supported

- **SVCs:** `ConnectToNamedPort` (only "sm:"), `SendSyncRequest`, `SendSyncRequestWithUserBuffer`, `CloseHandle`.
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

## What a libnx homebrew needs next

libnx's default start-up (`__appInit`) calls, in order: `sm:` (done) -> `set:sys`
(done, or skipped thanks to the HosVersion loader entry) -> `appletOE`
(OpenApplicationProxy and its sub-interfaces) -> `hid` -> `time:u` -> `fsp-srv`.
Each one will show up as the next "no implementado" message.
