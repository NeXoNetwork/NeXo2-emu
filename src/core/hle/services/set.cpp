#include "set.hpp"
#include "hle/firmware.hpp"
#include "hle/kernel.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <vector>

namespace NeXo2::HLE {

namespace {
// struct FirmwareVersion (0x100 bytes), igual que SetSysFirmwareVersion de libnx
struct FirmwareVersion {
    u8   major, minor, micro, padding1;
    u8   revision_major, revision_minor, padding2, padding3;
    char platform[0x20];
    char version_hash[0x40];
    char display_version[0x18];
    char display_title[0x80];
};
static_assert(sizeof(FirmwareVersion) == 0x100, "FirmwareVersion debe medir 0x100 bytes");
} // namespace

SystemSettings::SystemSettings() : ServiceObject("set:sys") {
    RegisterCommand(3, "GetFirmwareVersion",  [this](IpcContext& ctx) { GetFirmwareVersion(ctx); });
    RegisterCommand(4, "GetFirmwareVersion2", [this](IpcContext& ctx) { GetFirmwareVersion(ctx); });
    // Tema del sistema: 0 = blanco basico, 1 = negro basico (el hbmenu elige sus colores con esto)
    RegisterCommand(23, "GetColorSetId", [](IpcContext& ctx) { ctx.Push<s32>(1); ctx.SetResult(Result::Success); });
}

// Idiomas de la consola, en el orden de libnx (SetLanguage). El codigo es el texto
// ("es", "en-US"...) metido en un u64.
namespace {
constexpr const char* LANGUAGE_CODES[] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
    "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR",
};
u64 LanguageCode(const char* text) {
    u64 code = 0;
    std::memcpy(&code, text, std::min<size_t>(std::strlen(text), 8));
    return code;
}
constexpr const char* SYSTEM_LANGUAGE = "es";   // espanol de Espana
constexpr s32 REGION_EUROPE = 2;
} // namespace

Settings::Settings() : ServiceObject("set") {
    RegisterCommand(0, "GetLanguageCode", [](IpcContext& ctx) {
        ctx.Push<u64>(LanguageCode(SYSTEM_LANGUAGE));
        ctx.SetResult(Result::Success);
    });
    // GetAvailableLanguageCodes (buffer A) / ...2 (buffer B): lista de codigos -> cuantos
    auto codes = [](size_t max) {
        return [max](IpcContext& ctx) {
            std::vector<u64> list;
            for (const char* c : LANGUAGE_CODES) if (list.size() < max) list.push_back(LanguageCode(c));
            const size_t room = ctx.GetWriteBufferSize(0) / 8;
            if (list.size() > room) list.resize(room);
            ctx.WriteBuffer(list.data(), list.size() * 8, 0);
            ctx.Push<s32>(s32(list.size()));
            ctx.SetResult(Result::Success);
        };
    };
    RegisterCommand(1, "GetAvailableLanguageCodes", codes(15));
    RegisterCommand(5, "GetAvailableLanguageCodes2", codes(std::size(LANGUAGE_CODES)));
    RegisterCommand(2, "MakeLanguageCode", [](IpcContext& ctx) {
        const s32 lang = ctx.Pop<s32>();
        if (lang < 0 || size_t(lang) >= std::size(LANGUAGE_CODES)) { ctx.SetResult(0x3C69); return; }
        ctx.Push<u64>(LanguageCode(LANGUAGE_CODES[lang]));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(3, "GetAvailableLanguageCodeCount", [](IpcContext& ctx) { ctx.Push<s32>(15); ctx.SetResult(Result::Success); });
    RegisterCommand(6, "GetAvailableLanguageCodeCount2", [](IpcContext& ctx) {
        ctx.Push<s32>(s32(std::size(LANGUAGE_CODES)));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(4, "GetRegionCode", [](IpcContext& ctx) { ctx.Push<s32>(REGION_EUROPE); ctx.SetResult(Result::Success); });
}

// GetFirmwareVersion() -> buffer de salida con FirmwareVersion
void SystemSettings::GetFirmwareVersion(IpcContext& ctx) {
    FirmwareVersion fw{};
    fw.major = Firmware::MAJOR;
    fw.minor = Firmware::MINOR;
    fw.micro = Firmware::MICRO;
    fw.revision_major = 1;
    std::snprintf(fw.platform, sizeof(fw.platform), "NX");
    std::snprintf(fw.version_hash, sizeof(fw.version_hash), "nexo2-hle");
    std::snprintf(fw.display_version, sizeof(fw.display_version), "%s", Firmware::DISPLAY_VERSION);
    std::snprintf(fw.display_title, sizeof(fw.display_title), "NeXo 2 HLE Firmware %s", Firmware::DISPLAY_VERSION);
    ctx.WriteBuffer(&fw, sizeof(fw), 0);
    ctx.SetResult(Result::Success);
}

} // namespace NeXo2::HLE
