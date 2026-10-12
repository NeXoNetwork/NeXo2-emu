#include "applet.hpp"
#include "hle/kernel.hpp"
#include "hle/kernel_objects.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include "game.hpp"

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
constexpr u32 RESULT_NO_DATA_IN_CHANNEL = 0x480;   // AM: el canal esta vacio (modulo 128, desc 2)
constexpr u32 RESULT_OUT_OF_RANGE = 0x1F680;        // AM: lectura/escritura fuera del storage (desc 503)

// IStorage de AM: un bloque de bytes que se pasan programas y applets (parametros de arranque...)
class StorageAccessor final : public LenientService {
public:
    explicit StorageAccessor(std::shared_ptr<std::vector<u8>> data) : LenientService("am:IStorageAccessor") {
        RegisterCommand(0, "GetSize", [data](IpcContext& ctx) { ctx.Push<u64>(data->size()); ctx.SetResult(Result::Success); });
        RegisterCommand(10, "Write", [data](IpcContext& ctx) {
            const u64 offset = ctx.Pop<u64>();
            const auto in = ctx.ReadBuffer(0);
            if (offset > data->size() || in.size() > data->size() - offset) { ctx.SetResult(RESULT_OUT_OF_RANGE); return; }
            if (!in.empty()) std::memcpy(data->data() + offset, in.data(), in.size());
            ctx.SetResult(Result::Success);
        });
        RegisterCommand(11, "Read", [data](IpcContext& ctx) {
            const u64 offset = ctx.Pop<u64>();
            const u64 want = ctx.GetWriteBufferSize(0);
            if (offset > data->size()) { ctx.SetResult(RESULT_OUT_OF_RANGE); return; }
            const u64 n = std::min<u64>(want, data->size() - offset);
            if (n) ctx.WriteBuffer(data->data() + offset, size_t(n), 0);
            ctx.SetResult(Result::Success);
        });
    }
};

class Storage final : public LenientService {
public:
    explicit Storage(std::vector<u8> bytes) : LenientService("am:IStorage") {
        auto data = std::make_shared<std::vector<u8>>(std::move(bytes));
        RegisterCommand(0, "Open", [data](IpcContext& ctx) {
            ctx.PushInterface(std::make_shared<StorageAccessor>(data)); ctx.SetResult(Result::Success); });
    }
};

class LibraryAppletCreator final : public LenientService {
public:
    LibraryAppletCreator() : LenientService("ILibraryAppletCreator") {
        RegisterCommand(10, "CreateStorage", [](IpcContext& ctx) {
            const s64 size = ctx.Pop<s64>();
            ctx.PushInterface(std::make_shared<Storage>(std::vector<u8>(size_t(std::max<s64>(size, 0)))));
            ctx.SetResult(Result::Success);
        });
    }
};
constexpr u8  FOCUS_STATE_IN_FOCUS = 1;     // el programa esta en primer plano
constexpr u8  OPERATION_MODE_HANDHELD = 0;  // portatil
constexpr u8  OPERATION_MODE_DOCKED = 1;    // conectada al dock (TV)
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
    RegisterCommand(11,   "GetLibraryAppletCreator", Open<LibraryAppletCreator>());
    RegisterCommand(20,   "GetApplicationFunctions", Open<ApplicationFunctions>());
    RegisterCommand(1000, "GetDebugFunctions",       Open<EmptyInterface>(std::string("IDebugFunctions")));
}

CommonStateGetter::CommonStateGetter() : LenientService("ICommonStateGetter") {
    // Cola de mensajes del applet. Al arrancar, la consola manda "FocusStateChanged" (15): los
    // juegos (nn::oe) lo esperan antes de empezar
    auto queue = std::make_shared<std::deque<u32>>(std::deque<u32>{15});
    auto event = std::make_shared<KEvent>("mensajes del applet", true);
    RegisterCommand(0, "GetEventHandle", [event](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().Handles().Create(event));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "ReceiveMessage", [queue, event](IpcContext& ctx) {
        if (queue->empty()) { event->signaled = false; ctx.SetResult(RESULT_NO_MESSAGES); return; }
        ctx.Push<u32>(queue->front());
        queue->pop_front();
        if (queue->empty()) event->signaled = false;
        ctx.SetResult(Result::Success);
    });
    // 0 = portatil, 1 = dock (Ajustes -> General -> Modo de la consola)
    RegisterCommand(5, "GetOperationMode", [](IpcContext& ctx) {
        ctx.Push<u8>(ctx.GetKernel().IsDocked() ? OPERATION_MODE_DOCKED : OPERATION_MODE_HANDHELD);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(6, "GetPerformanceMode", [](IpcContext& ctx) {   // 0 normal, 1 boost (dock)
        ctx.Push<u32>(ctx.GetKernel().IsDocked() ? 1 : 0);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(9, "GetCurrentFocusState", Reply<u8>(FOCUS_STATE_IN_FOCUS));
    RegisterCommand(60, "GetDefaultDisplayResolution", [](IpcContext& ctx) {
        ctx.Push<s32>(s32(ctx.GetKernel().DisplayWidth()));
        ctx.Push<s32>(s32(ctx.GetKernel().DisplayHeight()));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(61, "GetDefaultDisplayResolutionChangeEvent", NewEvent("cambio de resolucion"));
    RegisterStub(66, "SetCpuBoostMode");   // modo turbo de la CPU (cargas): nada que cambiar aqui
    RegisterCommand(8, "GetBootMode", Reply<u8>(0));
    RegisterCommand(50, "IsVrModeEnabled", Reply<u8>(0));
    RegisterCommand(55, "IsInControllerFirmwareUpdateSection", Reply<u8>(0));
    RegisterCommand(62, "GetHdcpAuthenticationState", Reply<s32>(1));
    RegisterCommand(63, "GetHdcpAuthenticationStateChangeEvent", NewEvent("hdcp"));
    RegisterCommand(300, "GetSettingsPlatformRegion", Reply<u8>(1));
}

SelfController::SelfController() : LenientService("ISelfController") {
    RegisterStub(0,  "Exit");
    RegisterStub(1,  "LockExit");
    RegisterStub(2,  "UnlockExit");
    RegisterStub(10, "SetScreenShotPermission");
    RegisterStub(11, "SetOperationModeChangedNotification");
    RegisterStub(12, "SetPerformanceModeChangedNotification");
    RegisterStub(13, "SetFocusHandlingMode");
    RegisterStub(14, "SetRestartMessageEnabled");
    RegisterStub(16, "SetOutOfFocusSuspendingEnabled");
    RegisterStub(3,  "EnterFatalSection");
    RegisterStub(4,  "LeaveFatalSection");
    // Evento "ya se puede lanzar un applet" (teclado, errores...): siempre listo
    RegisterCommand(9, "GetLibraryAppletLaunchableEvent", [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent("applet lanzable", true));
        ctx.SetResult(Result::Success);
    });
    RegisterStub(15, "SetScreenShotAppletIdentityInfo");
    RegisterStub(17, "SetControllerFirmwareUpdateSection");
    RegisterStub(18, "SetRequiresCaptureButtonShortPressedMessage");
    RegisterStub(19, "SetAlbumImageOrientation");
    RegisterCommand(40, "CreateManagedDisplayLayer", Reply<u64>(1)); // id de la capa
    RegisterStub(50, "SetHandlesRequestToDisplay");
    RegisterStub(62, "SetIdleTimeDetectionExtension");
    RegisterCommand(63, "GetIdleTimeDetectionExtension", Reply<u32>(0));
    RegisterStub(65, "ReportUserIsActive");
    RegisterStub(68, "SetAutoSleepDisabled");
    RegisterCommand(69, "IsAutoSleepDisabled", Reply<u8>(0));
    RegisterCommand(91, "GetAccumulatedSuspendedTickChangedEvent", NewEvent("tiempo suspendido"));
    RegisterStub(100, "SetAlbumImageTakenNotificationEnabled");
}

WindowController::WindowController() : LenientService("IWindowController") {
    RegisterCommand(1, "GetAppletResourceUserId", Reply<u64>(APPLET_RESOURCE_USER_ID));
    RegisterStub(10, "AcquireForegroundRights");
}

ApplicationFunctions::ApplicationFunctions() : LenientService("IApplicationFunctions") {
    // Idioma: codigo empaquetado en un u64, igual que set ("es-ES")
    RegisterCommand(21, "GetDesiredLanguage", Reply<u64>(0x53452D7365ULL)); // "es-ES"
    RegisterStub(22, "SetTerminateResult");
    RegisterCommand(40, "NotifyRunning", Reply<u8>(1));
    RegisterStub(66, "InitializeGamePlayRecording");
    RegisterStub(67, "SetGamePlayRecordingState");
    // Juegos comerciales
    // PopLaunchParameter(tipo): 1 = canal del usuario (vacio), 2 = usuario elegido al arrancar.
    // El de tipo 2 se da una sola vez: magic 0xC79497CA, "hay usuario elegido" (u32), uid
    auto user_given = std::make_shared<bool>(false);
    RegisterCommand(1, "PopLaunchParameter", [user_given](IpcContext& ctx) {
        const u32 kind = ctx.Pop<u32>();
        if (kind != 2 || *user_given) { ctx.SetResult(RESULT_NO_DATA_IN_CHANNEL); return; }
        *user_given = true;
        std::vector<u8> p(0x88, 0);
        const u32 magic = 0xC79497CA, selected = 1;
        std::memcpy(&p[0], &magic, 4); std::memcpy(&p[4], &selected, 4);
        std::memcpy(&p[8], &USER_ID_LO, 8); std::memcpy(&p[0x10], &USER_ID_HI, 8);
        ctx.PushInterface(std::make_shared<Storage>(std::move(p)));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(19, "CreateCacheStorage", [](IpcContext& ctx) { ctx.Push<u64>(0); ctx.Push<u64>(0); ctx.SetResult(Result::Success); });
    RegisterCommand(20, "EnsureSaveData", Reply<u64>(0));
    RegisterCommand(23, "GetDisplayVersion", [](IpcContext& ctx) {
        char version[0x10] = "1.0.0";
        ctx.PushBytes(version, sizeof(version));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(25, "ExtendSaveData", Reply<u64>(0));
    RegisterCommand(26, "GetSaveDataSize", [](IpcContext& ctx) {
        ctx.Push<u64>(64ull << 20); ctx.Push<u64>(64ull << 20);   // datos y diario: 64 MB
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(50, "GetPseudoDeviceId", [](IpcContext& ctx) { ctx.Push<u64>(0x4E65586F32ULL); ctx.Push<u64>(1); ctx.SetResult(Result::Success); });
    RegisterStub(90, "EnableApplicationCrashReport");
    RegisterStub(100, "InitializeApplicationCopyrightFrameBuffer");
    RegisterStub(101, "SetApplicationCopyrightImage");
    RegisterStub(102, "SetApplicationCopyrightVisibility");
    RegisterCommand(110, "QueryApplicationPlayStatistics", Reply<u32>(0));
    RegisterCommand(123, "GetPreviousProgramIndex", Reply<s32>(-1));
    RegisterCommand(130, "GetGpuErrorDetectedSystemEvent", NewEvent("error de la GPU"));
    RegisterCommand(140, "GetFriendInvitationStorageChannelEvent", NewEvent("invitaciones"));
    RegisterCommand(150, "TryPopFromNotificationStorageChannel", [](IpcContext& ctx) { ctx.SetResult(RESULT_NO_MESSAGES); });
    RegisterCommand(151, "GetNotificationStorageChannelEvent", NewEvent("notificaciones"));
    RegisterStub(1001, "PrepareForJit");
}

} // namespace NeXo2::HLE
