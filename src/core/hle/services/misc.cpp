#include "misc.hpp"
#include "hle/kernel.hpp"
#include "common/logger.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace NeXo2::HLE {

namespace {
constexpr u32 BATTERY_PERCENT = 100;
constexpr u32 CHARGER_TYPE_CHARGER = 1;   // 0 = sin cargador, 1 = cargador oficial
}

PsmService::PsmService() : ServiceObject("psm") {
    RegisterCommand(0, "GetBatteryChargePercentage", [](IpcContext& ctx) {
        ctx.Push<u32>(BATTERY_PERCENT);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "GetChargerType", [](IpcContext& ctx) {
        ctx.Push<u32>(CHARGER_TYPE_CHARGER);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(7, "OpenSession", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<PsmSession>());
        ctx.SetResult(Result::Success);
    });
}

PsmSession::PsmSession() : ServiceObject("IPsmSession") {
    // Evento que se activaria al cambiar la bateria o el cargador (aqui nunca cambian)
    RegisterCommand(0, "BindStateChangeEvent", [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent("psm: cambio de estado"));
        ctx.SetResult(Result::Success);
    });
    RegisterStub(1, "UnbindStateChangeEvent");
    RegisterStub(2, "SetChargerTypeChangeEventEnabled");
    RegisterStub(3, "SetPowerSupplyChangeEventEnabled");
    RegisterStub(4, "SetBatteryVoltageStateChangeEventEnabled");
}

namespace {
constexpr u32 BSD_ENETDOWN = 50;          // errno de FreeBSD (el de Horizon)
constexpr u32 RESULT_NIFM_NO_NETWORK = 0x2A6E;   // nifm: sin conexion (modulo 110)
}

BsdService::BsdService(std::string name) : ServiceObject(std::move(name)) {
    // RegisterClient(config, pid, tamano de la memoria de transferencia, handle) -> u64 pid
    RegisterCommand(0, "RegisterClient", [](IpcContext& ctx) { ctx.Push<u64>(0); ctx.SetResult(Result::Success); });
    RegisterStub(1, "StartMonitoring");
    // Todo lo demas: -1 y errno = ENETDOWN
    static const std::pair<u32, const char*> ops[] = {
        {2, "Socket"}, {3, "SocketExempt"}, {4, "Open"}, {5, "Select"}, {6, "Poll"}, {7, "Sysctl"},
        {8, "Recv"}, {9, "RecvFrom"}, {10, "Send"}, {11, "SendTo"}, {12, "Accept"}, {13, "Bind"},
        {14, "Connect"}, {15, "GetPeerName"}, {16, "GetSockName"}, {17, "GetSockOpt"}, {18, "Listen"},
        {19, "Ioctl"}, {20, "Fcntl"}, {21, "SetSockOpt"}, {22, "Shutdown"}, {23, "ShutdownAllSockets"},
        {24, "Write"}, {25, "Read"}, {26, "Close"}, {27, "DuplicateSocket"},
    };
    for (const auto& [id, op] : ops) {
        RegisterCommand(id, op, [](IpcContext& ctx) {
            ctx.Push<s32>(-1);
            ctx.Push<u32>(BSD_ENETDOWN);
            ctx.SetResult(Result::Success);
        });
    }
}

NifmService::NifmService(std::string name) : ServiceObject(std::move(name)) {
    auto create = [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<NifmGeneralService>());
        ctx.SetResult(Result::Success);
    };
    RegisterCommand(4, "CreateGeneralServiceOld", create);
    RegisterCommand(5, "CreateGeneralService", create);
}

NifmGeneralService::NifmGeneralService() : ServiceObject("IGeneralService") {
    RegisterCommand(1, "GetClientId", [](IpcContext& ctx) { ctx.Push<u32>(1); ctx.SetResult(Result::Success); });
    // GetCurrentIpAddress: sin red
    RegisterCommand(12, "GetCurrentIpAddress", [](IpcContext& ctx) { ctx.SetResult(RESULT_NIFM_NO_NETWORK); });
    // GetInternetConnectionStatus: sin red
    RegisterCommand(18, "GetInternetConnectionStatus", [](IpcContext& ctx) { ctx.SetResult(RESULT_NIFM_NO_NETWORK); });
    RegisterCommand(21, "IsAnyInternetRequestAccepted", [](IpcContext& ctx) { ctx.Push<u8>(0); ctx.SetResult(Result::Success); });
}

namespace {
constexpr s32 TEMPERATURE_C = 35;
}

TsService::TsService() : ServiceObject("ts") {
    RegisterCommand(1, "GetTemperature", [](IpcContext& ctx) { ctx.Push<s32>(TEMPERATURE_C); ctx.SetResult(Result::Success); });
    RegisterStub(2, "SetMeasurementMode");
    RegisterCommand(3, "GetTemperatureMilliC", [](IpcContext& ctx) { ctx.Push<s32>(TEMPERATURE_C * 1000); ctx.SetResult(Result::Success); });
    RegisterCommand(4, "OpenSession", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<TsSession>());
        ctx.SetResult(Result::Success);
    });
}

TsSession::TsSession() : ServiceObject("ts:ISession") {
    RegisterStub(2, "SetMeasurementMode");
    RegisterCommand(4, "GetTemperature", [](IpcContext& ctx) { ctx.Push<float>(float(TEMPERATURE_C)); ctx.SetResult(Result::Success); });
}

namespace {
constexpr size_t PL_SHARED_MEMORY_SIZE = 0x1100000;   // lo que mapea libnx (plInitialize)
constexpr u32 PL_FONT_TYPES = 6;   // estandar, chino simpl., chino simpl. ext., chino trad., coreano, Nintendo ext.
constexpr u32 PL_FONT_OFFSET = 8;  // la fuente va detras de una cabecera de 8 bytes (magia, tamano)

// La fuente del PC que hace de fuente del sistema (se lee una vez)
const std::vector<u8>& HostFont() {
    static const std::vector<u8> font = [] {
        std::vector<std::filesystem::path> candidates;
        if (const char* env = std::getenv("NEXO2_FONT")) candidates.emplace_back(env);
        if (const char* windir = std::getenv("WINDIR")) {
            candidates.emplace_back(std::filesystem::path(windir) / "Fonts" / "segoeui.ttf");
            candidates.emplace_back(std::filesystem::path(windir) / "Fonts" / "arial.ttf");
        }
        candidates.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        candidates.emplace_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
        for (const auto& p : candidates) {
            std::ifstream f(p, std::ios::binary);
            if (!f) continue;
            std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (data.size() < 12 || data.size() + PL_FONT_OFFSET > PL_SHARED_MEMORY_SIZE) continue;
            const auto u8p = p.u8string();
            Common::Logger::Log(Common::Logger::Level::Info, "[pl:u] Fuente del sistema: " + std::string(u8p.begin(), u8p.end()));
            return data;
        }
        Common::Logger::Log(Common::Logger::Level::Warning,
            "[pl:u] No hay ninguna fuente TrueType: pon NEXO2_FONT=ruta\\a\\una.ttf");
        return std::vector<u8>{};
    }();
    return font;
}
u32 FontSize() { return u32(HostFont().size()); }
}

PlService::PlService() : ServiceObject("pl:u") {
    RegisterStub(0, "RequestLoad");
    RegisterCommand(1, "GetLoadState", [](IpcContext& ctx) { ctx.Push<u32>(HostFont().empty() ? 0 : 1); ctx.SetResult(Result::Success); });
    RegisterCommand(2, "GetSize", [](IpcContext& ctx) { ctx.Push<u32>(FontSize()); ctx.SetResult(Result::Success); });
    // Todas las fuentes son la misma, en la misma posicion
    RegisterCommand(3, "GetSharedMemoryAddressOffset", [](IpcContext& ctx) { ctx.Push<u32>(PL_FONT_OFFSET); ctx.SetResult(Result::Success); });
    RegisterCommand(4, "GetSharedMemoryNativeHandle", [](IpcContext& ctx) {
        std::shared_ptr<KSharedMemory> shmem;
        const u32 handle = ctx.GetKernel().CreateSharedMemory("pl:u fuentes", PL_SHARED_MEMORY_SIZE, &shmem);
        const auto& font = HostFont();
        // Cabecera como la de la consola (magia y tamano cifrados con XOR); el hbmenu solo usa los datos
        constexpr u32 KEY = 0x49621806, MAGIC = 0x36F81A1E;
        const u32 header[2] = {MAGIC ^ KEY, u32(font.size()) ^ KEY};
        std::memcpy(shmem->data.data(), header, sizeof(header));
        if (!font.empty()) std::memcpy(shmem->data.data() + PL_FONT_OFFSET, font.data(), font.size());
        ctx.PushCopyHandle(handle);
        ctx.SetResult(Result::Success);
    });
    // GetSharedFontInOrderOfPriority(u64 idioma) -> u8 cargadas, u32 cuantas + listas de tipos,
    // posiciones y tamanos (3 buffers de u32)
    RegisterCommand(5, "GetSharedFontInOrderOfPriority", [](IpcContext& ctx) {
        std::vector<u32> types, offsets, sizes;
        for (u32 t = 0; t < PL_FONT_TYPES; ++t) { types.push_back(t); offsets.push_back(PL_FONT_OFFSET); sizes.push_back(FontSize()); }
        ctx.WriteBuffer(types.data(), types.size() * 4, 0);
        ctx.WriteBuffer(offsets.data(), offsets.size() * 4, 1);
        ctx.WriteBuffer(sizes.data(), sizes.size() * 4, 2);
        ctx.Push<u8>(HostFont().empty() ? 0 : 1);
        ctx.Push<u8>(0); ctx.Push<u8>(0); ctx.Push<u8>(0);
        ctx.Push<u32>(PL_FONT_TYPES);
        ctx.SetResult(Result::Success);
    });
}

} // namespace NeXo2::HLE
