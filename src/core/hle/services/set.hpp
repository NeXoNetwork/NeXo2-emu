#pragma once
#include "hle/service.hpp"

namespace NeXo2::HLE {

// "set:sys": ajustes del sistema. libnx lo usa al arrancar para saber la
// version de firmware (si el loader no se la ha dado).
// Referencia: https://switchbrew.org/wiki/Settings_services
class SystemSettings final : public ServiceObject {
public:
    SystemSettings();

private:
    void GetFirmwareVersion(IpcContext& ctx);
};

} // namespace NeXo2::HLE
