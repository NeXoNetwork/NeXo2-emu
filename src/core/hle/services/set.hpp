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

// "set": ajustes que puede leer cualquier programa (idioma y region de la consola).
class Settings final : public ServiceObject {
public:
    Settings();
};

} // namespace NeXo2::HLE
