#pragma once
#include "common/types.hpp"

// Version de firmware que NeXo dice tener (la que ven los programas por
// set:sys y por la entrada HosVersion del loader de homebrew).
// Lista de versiones reales: docs/05-switch2-system/firmware-versions.md
namespace NeXo2::HLE::Firmware {
    constexpr u8 MAJOR = 20;
    constexpr u8 MINOR = 1;
    constexpr u8 MICRO = 0;
    constexpr const char* DISPLAY_VERSION = "20.1.0";

    // Formato de libnx: MAKEHOSVERSION(major, minor, micro)
    constexpr u32 HOS_VERSION = (u32(MAJOR) << 16) | (u32(MINOR) << 8) | MICRO;
}
