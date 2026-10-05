#include "set.hpp"
#include "hle/firmware.hpp"
#include "hle/kernel.hpp"
#include <cstdio>
#include <cstring>

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
