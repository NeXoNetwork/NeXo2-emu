#pragma once
#include <map>
#include <string>
#include "hle/service.hpp"

// "nvdrv": el driver de la GPU de NVIDIA. Funciona como un Linux: el programa
// abre "dispositivos" (/dev/nvmap, /dev/nvhost-gpu...) y les manda ioctls.
// Referencia: https://switchbrew.org/wiki/NV_services y libnx (nvidia/ioctl.h).
//
//   /dev/nvmap           bloques de memoria del programa que la GPU puede usar
//   /dev/nvhost-ctrl     syncpoints y eventos (esperar a que la GPU termine)
//   /dev/nvhost-ctrl-gpu informacion de la GPU (modelo, clases, zcull...)
//   /dev/nvhost-as-gpu   espacio de direcciones de la GPU: proyectar bloques nvmap
//   /dev/nvhost-gpu      canal de comandos: GPFIFO -> video_core/gpu.hpp
// Ver docs/07-nexo-internals/gpu.md.
namespace NeXo2::HLE {

class NvDrv final : public ServiceObject {
public:
    // 'tipc': la misma interfaz por TIPC (la sesion nvsrv del sistema grafico de Switch 2)
    explicit NvDrv(std::string name, bool tipc = false);
    bool SupportsTipc() const override { return m_tipc; }

private:
    bool m_tipc = false;
    enum class Device { NvMap, Ctrl, CtrlGpu, AsGpu, Gpu, Other };
    struct File {
        Device device;
        std::string path;
        u32 channel = ~0u;    // /dev/nvhost-gpu: canal de la GPU
    };

    void Open(IpcContext& ctx);
    // version: 1 = Ioctl, 2 = Ioctl2 (entrada extra), 3 = Ioctl3 (salida extra)
    void Ioctl(IpcContext& ctx, int version);
    void Close(IpcContext& ctx);
    void QueryEvent(IpcContext& ctx);
    u32 UnknownIoctl(IpcContext& ctx, const std::string& device, u32 request, std::vector<u8>& d);
    std::vector<u8> m_inline;      // Ioctl3: segundo buffer de salida
    bool m_inlineUsed = false;

    // Cada ioctl recibe sus datos (entrada y salida en el mismo buffer) y devuelve
    // el codigo de error de NVIDIA (0 = bien). 'extra' = buffer adicional de Ioctl2/3.
    u32 IoctlNvMap(IpcContext& ctx, u32 request, std::vector<u8>& data);
    u32 IoctlCtrl(IpcContext& ctx, u32 request, std::vector<u8>& data);
    u32 IoctlCtrlGpu(IpcContext& ctx, u32 request, std::vector<u8>& data);
    u32 IoctlAsGpu(IpcContext& ctx, u32 request, std::vector<u8>& data);
    u32 IoctlGpu(IpcContext& ctx, File& file, u32 request, std::vector<u8>& data, const std::vector<u8>& extra);

    std::map<u32, File> m_fds;
    u32 m_nextFd = 1;
};

} // namespace NeXo2::HLE
