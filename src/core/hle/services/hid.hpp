#pragma once
#include "hle/service.hpp"

// "hid": mandos, pantalla tactil, teclado, raton...
// El estado de los mandos NO se pide por IPC: el servicio entrega una memoria
// compartida de 0x40000 bytes que el programa lee directamente.
// Referencia: https://switchbrew.org/wiki/HID_services
namespace NeXo2::HLE {

class HidServer final : public ServiceObject { public: HidServer(); };
class HidAppletResource final : public ServiceObject { public: HidAppletResource(); };

constexpr size_t HID_SHARED_MEMORY_SIZE = 0x40000;

} // namespace NeXo2::HLE
