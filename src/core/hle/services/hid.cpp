#include "hid.hpp"
#include "hle/kernel.hpp"

namespace NeXo2::HLE {

HidServer::HidServer() : ServiceObject("hid") {
    // CreateAppletResource(AppletResourceUserId, pid) -> IAppletResource
    RegisterCommand(0, "CreateAppletResource", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<HidAppletResource>());
        ctx.SetResult(Result::Success);
    });
    // "Activar" dispositivos y configurar mandos: de momento solo respondemos OK.
    // (padConfigureInput, hidInitializeTouchScreen... de libnx llaman a estos)
    RegisterStub(1,   "ActivateDebugPad");
    RegisterStub(11,  "ActivateTouchScreen");
    RegisterStub(21,  "ActivateMouse");
    RegisterStub(31,  "ActivateKeyboard");
    RegisterStub(66,  "StartSixAxisSensor");
    RegisterStub(100, "SetSupportedNpadStyleSet");
    RegisterCommand(101, "GetSupportedNpadStyleSet", [](IpcContext& ctx) {
        ctx.Push<u32>(0x1F); // FullKey, Handheld, JoyDual, JoyLeft, JoyRight
        ctx.SetResult(Result::Success);
    });
    RegisterStub(102, "SetSupportedNpadIdType");
    RegisterStub(103, "ActivateNpad");
    RegisterStub(109, "ActivateNpadWithRevision");
    RegisterStub(120, "SetNpadJoyHoldType");
    RegisterStub(124, "SetNpadJoyAssignmentModeDual");
    RegisterStub(128, "SetNpadHandheldActivationMode");
    // Lo que piden los juegos comerciales al arrancar (sensores, vibracion, Joy-Con sueltos):
    // se acepta todo; los sensores dan reposo y la vibracion no hace nada
    for (u32 id : {67u, 69u, 70u, 72u, 73u, 74u, 75u, 77u, 79u, 81u, 84u, 85u, 87u, 88u})
        RegisterStub(id, "SixAxisSensor (sin sensor)");
    RegisterCommand(68, "IsSixAxisSensorFusionEnabled", [](IpcContext& ctx) { ctx.Push<u8>(0); ctx.SetResult(Result::Success); });
    RegisterCommand(82, "IsSixAxisSensorAtRest", [](IpcContext& ctx) { ctx.Push<u8>(1); ctx.SetResult(Result::Success); });
    RegisterCommand(106, "AcquireNpadStyleSetUpdateEventHandle", [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent("hid: estilo de mando", true));
        ctx.SetResult(Result::Success);
    });
    RegisterStub(107, "DisconnectNpad");
    RegisterCommand(108, "GetPlayerLedPattern", [](IpcContext& ctx) { ctx.Push<u64>(1); ctx.SetResult(Result::Success); });
    RegisterCommand(121, "GetNpadJoyHoldType", [](IpcContext& ctx) { ctx.Push<u64>(1); ctx.SetResult(Result::Success); });
    for (u32 id : {122u, 123u, 125u, 126u, 127u, 130u, 133u, 134u, 135u})
        RegisterStub(id, "NpadJoyAssignment");
    RegisterCommand(129, "GetNpadHandheldActivationMode", [](IpcContext& ctx) { ctx.Push<u64>(0); ctx.SetResult(Result::Success); });
    RegisterCommand(200, "GetVibrationDeviceInfo", [](IpcContext& ctx) {
        ctx.Push<u32>(1);   // tipo: motor lineal
        ctx.Push<u32>(1);   // posicion
        ctx.SetResult(Result::Success);
    });
    RegisterStub(201, "SendVibrationValue");
    RegisterCommand(202, "GetActualVibrationValue", [](IpcContext& ctx) {
        ctx.Push<float>(0); ctx.Push<float>(160); ctx.Push<float>(0); ctx.Push<float>(320);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(203, "CreateActiveVibrationDeviceList", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<ActiveVibrationDeviceList>());
        ctx.SetResult(Result::Success);
    });
    RegisterStub(204, "PermitVibration");
    RegisterCommand(205, "IsVibrationPermitted", [](IpcContext& ctx) { ctx.Push<u8>(1); ctx.SetResult(Result::Success); });
    RegisterStub(206, "SendVibrationValues");
    RegisterStub(207, "SendVibrationGcErmCommand");
    RegisterCommand(211, "IsVibrationDeviceMounted", [](IpcContext& ctx) { ctx.Push<u8>(1); ctx.SetResult(Result::Success); });
    for (u32 id : {300u, 301u, 302u, 303u, 304u, 305u, 306u, 307u, 308u})
        RegisterStub(id, "ConsoleSixAxisSensor (sin sensor)");
    RegisterStub(1000, "SetNpadCommunicationMode");
    RegisterCommand(1001, "GetNpadCommunicationMode", [](IpcContext& ctx) { ctx.Push<u64>(0); ctx.SetResult(Result::Success); });
    RegisterStub(1002, "SetTouchScreenConfiguration");
}

ActiveVibrationDeviceList::ActiveVibrationDeviceList() : ServiceObject("hid:IActiveVibrationDeviceList") {
    RegisterStub(0, "ActivateVibrationDevice");
}

HidAppletResource::HidAppletResource() : ServiceObject("hid:IAppletResource") {
    // GetSharedMemoryHandle -> handle "copy" de la memoria compartida de hid, con
    // dos mandos conectados (ver hle/input.hpp). La interfaz la actualiza con el teclado.
    RegisterCommand(0, "GetSharedMemoryHandle", [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().GetHidSharedMemoryHandle());
        ctx.SetResult(Result::Success);
    });
}

} // namespace NeXo2::HLE
