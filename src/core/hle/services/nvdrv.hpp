#pragma once
#include <map>
#include <string>
#include "hle/service.hpp"

// "nvdrv": el driver de la GPU de NVIDIA. Funciona como un Linux: el programa
// abre "dispositivos" (/dev/nvmap, /dev/nvhost-ctrl...) y les manda ioctls.
// Referencia: https://switchbrew.org/wiki/NV_services
//
// De momento solo lo necesario para mostrar imagenes por CPU (la consola de libnx):
//   /dev/nvmap        reservar memoria para buffers de imagen
//   /dev/nvhost-ctrl  sincronizacion (fences/eventos); solo lo basico
namespace NeXo2::HLE {

class NvDrv final : public ServiceObject {
public:
    explicit NvDrv(std::string name);

private:
    void Open(IpcContext& ctx);
    void Ioctl(IpcContext& ctx);
    void Close(IpcContext& ctx);

    // Cada ioctl recibe sus datos de entrada y devuelve los de salida (mismo tamano).
    // Devuelve el codigo de error de NVIDIA (0 = bien).
    u32 IoctlNvMap(IpcContext& ctx, u32 request, std::vector<u8>& data);
    u32 IoctlNvHostCtrl(IpcContext& ctx, u32 request, std::vector<u8>& data);

    std::map<u32, std::string> m_fds; // descriptor -> ruta del dispositivo
    u32 m_nextFd = 1;
};

} // namespace NeXo2::HLE
