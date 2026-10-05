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
