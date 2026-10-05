#include "applet.hpp"
#include "hle/kernel.hpp"

namespace NeXo2::HLE {

namespace {
// Devuelve una interfaz nueva de tipo T
template <typename T, typename... Args>
ServiceObject::Handler Open(Args... args) {
    return [=](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<T>(args...));
        ctx.SetResult(Result::Success);
    };
}

// Responde un valor fijo
template <typename T>
ServiceObject::Handler Reply(T value) {
    return [=](IpcContext& ctx) {
        ctx.Push<T>(value);
        ctx.SetResult(Result::Success);
    };
}

// Devuelve un evento nuevo (handle "copy")
ServiceObject::Handler NewEvent(const char* name) {
    return [=](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent(name));
        ctx.SetResult(Result::Success);
    };
}

constexpr u32 RESULT_NO_MESSAGES = 0x680;   // AM: no hay mensajes pendientes (modulo 128, desc 3)
constexpr u8  FOCUS_STATE_IN_FOCUS = 1;     // el programa esta en primer plano
constexpr u8  OPERATION_MODE_HANDHELD = 0;  // 0 = portatil, 1 = conectada al dock (TV)
} // namespace

AppletOE::AppletOE() : ServiceObject("appletOE") {
    // OpenApplicationProxy(pid, handle del proceso, u64 reservado) -> IApplicationProxy
    RegisterCommand(0, "OpenApplicationProxy", Open<ApplicationProxy>());
}

ApplicationProxy::ApplicationProxy() : ServiceObject("IApplicationProxy") {
    RegisterCommand(0,    "GetCommonStateGetter",    Open<CommonStateGetter>());
    RegisterCommand(1,    "GetSelfController",       Open<SelfController>());
    RegisterCommand(2,    "GetWindowController",     Open<WindowController>());
    RegisterCommand(3,    "GetAudioController",      Open<EmptyInterface>(std::string("IAudioController")));
    RegisterCommand(4,    "GetDisplayController",    Open<EmptyInterface>(std::string("IDisplayController")));
    RegisterCommand(11,   "GetLibraryAppletCreator", Open<EmptyInterface>(std::string("ILibraryAppletCreator")));
    RegisterCommand(20,   "GetApplicationFunctions", Open<ApplicationFunctions>());
    RegisterCommand(1000, "GetDebugFunctions",       Open<EmptyInterface>(std::string("IDebugFunctions")));
}

CommonStateGetter::CommonStateGetter() : ServiceObject("ICommonStateGetter") {
    RegisterCommand(0, "GetEventHandle", NewEvent("mensajes del applet"));
    RegisterCommand(1, "ReceiveMessage", [](IpcContext& ctx) { ctx.SetResult(RESULT_NO_MESSAGES); });
    RegisterCommand(5, "GetOperationMode",    Reply<u8>(OPERATION_MODE_HANDHELD));
    RegisterCommand(6, "GetPerformanceMode",  Reply<u32>(0));
    RegisterCommand(9, "GetCurrentFocusState", Reply<u8>(FOCUS_STATE_IN_FOCUS));
}

SelfController::SelfController() : ServiceObject("ISelfController") {
    RegisterStub(0,  "Exit");
    RegisterStub(1,  "LockExit");
    RegisterStub(2,  "UnlockExit");
    RegisterStub(10, "SetScreenShotPermission");
    RegisterStub(11, "SetOperationModeChangedNotification");
    RegisterStub(12, "SetPerformanceModeChangedNotification");
    RegisterStub(13, "SetFocusHandlingMode");
    RegisterStub(14, "SetRestartMessageEnabled");
    RegisterStub(16, "SetOutOfFocusSuspendingEnabled");
    RegisterStub(40, "CreateManagedDisplayLayer");
    RegisterCommand(91, "GetAccumulatedSuspendedTickChangedEvent", NewEvent("tiempo suspendido"));
}

WindowController::WindowController() : ServiceObject("IWindowController") {
    RegisterCommand(1, "GetAppletResourceUserId", Reply<u64>(APPLET_RESOURCE_USER_ID));
    RegisterStub(10, "AcquireForegroundRights");
}

ApplicationFunctions::ApplicationFunctions() : ServiceObject("IApplicationFunctions") {
    // Idioma: codigo empaquetado en un u64, igual que set ("es-ES")
    RegisterCommand(21, "GetDesiredLanguage", Reply<u64>(0x53452D7365ULL)); // "es-ES"
    RegisterStub(22, "SetTerminateResult");
    RegisterCommand(40, "NotifyRunning", Reply<u8>(1));
    RegisterStub(66, "InitializeGamePlayRecording");
    RegisterStub(67, "SetGamePlayRecordingState");
}

} // namespace NeXo2::HLE
