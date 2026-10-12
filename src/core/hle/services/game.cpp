#include "game.hpp"
#include <cstdio>
#include <cstring>
#include "avatar_jpeg.hpp"
#include "hle/kernel.hpp"
#include "common/logger.hpp"

namespace NeXo2::HLE {

using Common::Logger;

namespace {
template <typename T>
ServiceObject::Handler Reply(T value) {
    return [=](IpcContext& ctx) { ctx.Push<T>(value); ctx.SetResult(Result::Success); };
}
ServiceObject::Handler Event(const char* name, bool signaled = false) {
    return [=](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent(name, signaled));
        ctx.SetResult(Result::Success);
    };
}
template <typename T, typename... Args>
ServiceObject::Handler Open(Args... args) {
    return [=](IpcContext& ctx) { ctx.PushInterface(std::make_shared<T>(args...)); ctx.SetResult(Result::Success); };
}
void PushUid(IpcContext& ctx) { ctx.Push<u64>(USER_ID_LO); ctx.Push<u64>(USER_ID_HI); }

// Codigo de error de la Switch: 2XXX-YYYY (modulo + 2000, descripcion)
std::string ErrorCode(u32 result) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%04u", 2000 + (result & 0x1FF), (result >> 9) & 0x1FFF);
    return buf;
}

// IProfile: datos del usuario. ProfileBase = uid (0x10), fecha (u64), apodo (0x20)
class Profile final : public ServiceObject {
public:
    Profile() : ServiceObject("acc:IProfile") {
        auto base = [](IpcContext& ctx) {
            PushUid(ctx);
            ctx.Push<u64>(1700000000);
            char nick[0x20] = "NeXo";
            ctx.PushBytes(nick, sizeof(nick));
        };
        RegisterCommand(0, "Get", [base](IpcContext& ctx) {
            base(ctx);
            u8 user_data[0x80] = {};
            ctx.WriteBuffer(user_data, sizeof(user_data), 0);
            ctx.SetResult(Result::Success);
        });
        RegisterCommand(1, "GetBase", [base](IpcContext& ctx) { base(ctx); ctx.SetResult(Result::Success); });
        RegisterCommand(10, "GetImageSize", Reply<u32>(u32(sizeof(k_avatar_jpeg))));
        RegisterCommand(11, "LoadImage", [](IpcContext& ctx) {
            const size_t n = ctx.WriteBuffer(k_avatar_jpeg, sizeof(k_avatar_jpeg), 0);
            ctx.Push<u32>(u32(n));
            ctx.SetResult(Result::Success);
        });
    }
};

class ManagerForApplication final : public LenientService {
public:
    ManagerForApplication() : LenientService("acc:IManagerForApplication") {
        RegisterStub(0, "CheckAvailability");
        RegisterCommand(1, "GetAccountId", Reply<u64>(USER_ID_LO));
    }
};

class Logger_ final : public ServiceObject {
public:
    Logger_() : ServiceObject("lm:ILogger") {
        // Log: paquete = cabecera (0x18: pid, tid, flags, gravedad, tamano) + trozos (clave,
        // tamano en uleb128, datos). La clave 2 es el mensaje.
        RegisterCommand(0, "Log", [](IpcContext& ctx) {
            const auto buf = ctx.ReadBuffer(0);
            std::string text;
            size_t p = 0x18;
            while (p < buf.size()) {
                const u8 key = buf[p++];
                u64 len = 0;
                for (int shift = 0; p < buf.size() && shift < 35; shift += 7) {
                    const u8 b = buf[p++];
                    len |= u64(b & 0x7F) << shift;
                    if (!(b & 0x80)) break;
                }
                if (p + len > buf.size()) break;
                if (key == 2) text.append(reinterpret_cast<const char*>(&buf[p]), size_t(len));
                p += size_t(len);
            }
            while (!text.empty() && (text.back() == '\n' || text.back() == '\0')) text.pop_back();
            if (!text.empty()) Logger::Log(Logger::Level::Info, "[Juego] " + text);
            ctx.SetResult(Result::Success);
        });
        RegisterStub(1, "SetDestination");
    }
};

class ParentalControl final : public LenientService {
public:
    ParentalControl() : LenientService("pctl:IParentalControlService") {
        RegisterStub(1, "Initialize");
        RegisterStub(1001, "CheckFreeCommunicationPermission");
        RegisterCommand(1031, "IsRestrictionEnabled", Reply<u8>(0));
        RegisterCommand(1065, "IsStereoVisionPermitted", Reply<u8>(1));
    }
};

class FriendService final : public LenientService {
public:
    FriendService() : LenientService("friends:IFriendService") {
        RegisterCommand(10100, "GetFriendListIds", Reply<u32>(0));
        RegisterCommand(10101, "GetFriendList", Reply<u32>(0));
        RegisterStub(10600, "DeclareOpenOnlinePlaySession");
        RegisterStub(10601, "DeclareCloseOnlinePlaySession");
        RegisterStub(10610, "UpdateUserPresence");
    }
};

class FriendNotification final : public LenientService {
public:
    FriendNotification() : LenientService("friends:INotificationService") {
        RegisterCommand(0, "GetEvent", Event("amigos: avisos"));
        RegisterStub(1, "Clear");
        RegisterCommand(2, "Pop", [](IpcContext& ctx) { ctx.SetResult(0x6EC); });   // no hay avisos
    }
};

class PurchaseEventManager final : public LenientService {
public:
    PurchaseEventManager() : LenientService("aoc:IPurchaseEventManager") {
        RegisterCommand(2, "GetPurchasedEventReadableHandle", Event("aoc: compras"));
        RegisterCommand(3, "PopPurchasedProductInfo", [](IpcContext& ctx) { ctx.SetResult(0x1A0A); });
        RegisterCommand(4, "PopPurchasedProductInfoWithUid", [](IpcContext& ctx) { ctx.SetResult(0x1A0A); });
    }
};
} // namespace

LmService::LmService() : ServiceObject("lm") {
    RegisterCommand(0, "OpenLogger", Open<Logger_>());
}

AccountService::AccountService(std::string name) : ServiceObject(std::move(name)) {
    RegisterCommand(0, "GetUserCount", Reply<s32>(1));
    RegisterCommand(1, "GetUserExistence", Reply<u8>(1));
    auto list = [](IpcContext& ctx) {
        std::vector<u64> ids(size_t(ctx.GetWriteBufferSize(0) / 8), 0);
        if (ids.size() >= 2) { ids[0] = USER_ID_LO; ids[1] = USER_ID_HI; }
        if (!ids.empty()) ctx.WriteBuffer(ids.data(), ids.size() * 8, 0);
        ctx.SetResult(Result::Success);
    };
    RegisterCommand(2, "ListAllUsers", list);
    RegisterCommand(3, "ListOpenUsers", list);
    RegisterCommand(4, "GetLastOpenedUser", [](IpcContext& ctx) { PushUid(ctx); ctx.SetResult(Result::Success); });
    RegisterCommand(5, "GetProfile", Open<Profile>());
    RegisterCommand(6, "GetProfileDigest", [](IpcContext& ctx) { ctx.Push<u64>(0); ctx.Push<u64>(0); ctx.SetResult(Result::Success); });
    RegisterCommand(50, "IsUserRegistrationRequestPermitted", Reply<u8>(0));
    RegisterCommand(51, "TrySelectUserWithoutInteraction", [](IpcContext& ctx) { PushUid(ctx); ctx.SetResult(Result::Success); });
    RegisterCommand(60, "ListOpenContextStoredUsers", list);
    RegisterStub(100, "InitializeApplicationInfoV0");
    RegisterCommand(101, "GetBaasAccountManagerForApplication", Open<ManagerForApplication>());
    RegisterStub(102, "AuthenticateApplicationAsync");
    RegisterCommand(110, "StoreSaveDataThumbnail", [](IpcContext& ctx) { ctx.SetResult(Result::Success); });
    RegisterStub(111, "ClearSaveDataThumbnail");
    RegisterStub(140, "InitializeApplicationInfo");
    RegisterCommand(141, "ListQualifiedUsers", list);
    RegisterCommand(150, "IsUserAccountSwitchLocked", Reply<u8>(0));
}

PctlService::PctlService(std::string name) : LenientService(std::move(name)) {
    RegisterCommand(0, "CreateService", Open<ParentalControl>());
    RegisterCommand(1, "CreateServiceWithoutInitialize", Open<ParentalControl>());
}

FriendsService::FriendsService(std::string name) : LenientService(std::move(name)) {
    RegisterCommand(0, "CreateFriendService", Open<FriendService>());
    RegisterCommand(1, "CreateNotificationService", Open<FriendNotification>());
}

PrepoService::PrepoService(std::string name) : LenientService(std::move(name)) {}

AocService::AocService() : LenientService("aoc:u") {
    RegisterCommand(0, "CountAddOnContentByApplicationId", Reply<u32>(0));
    RegisterCommand(1, "ListAddOnContentByApplicationId", Reply<u32>(0));
    RegisterCommand(2, "CountAddOnContent", Reply<u32>(0));
    RegisterCommand(3, "ListAddOnContent", Reply<u32>(0));
    RegisterCommand(5, "GetAddOnContentBaseId", [](IpcContext& ctx) {
        ctx.Push<u64>((ctx.GetKernel().ProgramId() & ~0xFFFULL) + 0x1000);
        ctx.SetResult(Result::Success);
    });
    RegisterStub(7, "PrepareAddOnContent");
    RegisterCommand(8, "GetAddOnContentListChangedEvent", Event("aoc: lista"));
    RegisterCommand(10, "GetAddOnContentListChangedEventWithProcessId", Event("aoc: lista"));
    RegisterStub(11, "NotifyMountAddOnContent");
    RegisterStub(12, "NotifyUnmountAddOnContent");
    RegisterCommand(100, "CreateEcPurchasedEventManager", Open<PurchaseEventManager>());
    RegisterCommand(101, "CreatePermanentEcPurchasedEventManager", Open<PurchaseEventManager>());
}

FatalService::FatalService() : ServiceObject("fatal:u") {
    auto fatal = [](IpcContext& ctx) {
        const u32 result = u32(ctx.Pop<u64>());
        const std::string msg = "El juego ha dado un error fatal " + ErrorCode(result) + " (resultado 0x" +
                                [&] { char b[16]; std::snprintf(b, sizeof(b), "%X", result); return std::string(b); }() + ")";
        Logger::Log(Logger::Level::Error, "[fatal] " + msg);
        ctx.Unsupported(msg);
    };
    RegisterCommand(0, "ThrowFatal", fatal);
    RegisterCommand(1, "ThrowFatalWithPolicy", fatal);
    RegisterCommand(2, "ThrowFatalWithCpuContext", fatal);
}

} // namespace NeXo2::HLE
