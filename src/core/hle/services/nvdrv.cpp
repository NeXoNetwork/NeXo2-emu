#include "nvdrv.hpp"
#include "hle/kernel.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace NeXo2::HLE {

using Common::Logger;

namespace {
// Errores de NVIDIA (los devuelve el ioctl, no el resultado IPC)
constexpr u32 NV_SUCCESS       = 0;
constexpr u32 NV_NOT_SUPPORTED = 1;
constexpr u32 NV_BAD_VALUE     = 4;

// Campos del numero de ioctl (como en Linux): nr 8 bits, tipo 8, tamano 14, direccion 2
constexpr u32 IocNr(u32 r)   { return r & 0xFF; }
constexpr u32 IocType(u32 r) { return (r >> 8) & 0xFF; }
constexpr u32 IocSize(u32 r) { return (r >> 16) & 0x3FFF; }

template <typename T> T Get(const std::vector<u8>& d, size_t off) {
    T v{};
    if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}
template <typename T> void Put(std::vector<u8>& d, size_t off, T v) {
    if (off + sizeof(T) <= d.size()) std::memcpy(d.data() + off, &v, sizeof(T));
}

std::string Hex(u32 v) { char b[16]; std::snprintf(b, sizeof(b), "0x%08X", v); return b; }
} // namespace

NvDrv::NvDrv(std::string name) : ServiceObject(std::move(name)) {
    RegisterCommand(0, "Open",  [this](IpcContext& ctx) { Open(ctx); });
    RegisterCommand(1, "Ioctl", [this](IpcContext& ctx) { Ioctl(ctx); });
    RegisterCommand(2, "Close", [this](IpcContext& ctx) { Close(ctx); });
    // Initialize(tamano de la memoria de transferencia, handle del proceso, handle de la memoria)
    RegisterStub(3, "Initialize");
    RegisterCommand(4, "QueryEvent", [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent("evento nvdrv"));
        ctx.Push<u32>(NV_SUCCESS);
        ctx.SetResult(Result::Success);
    });
    RegisterStub(8, "SetAruid");
    RegisterStub(13, "SetGraphicsFirmwareMemoryMarginEnabled");
}

// Open(ruta en buffer A) -> u32 fd, u32 error
void NvDrv::Open(IpcContext& ctx) {
    const std::vector<u8> raw = ctx.ReadBuffer(0);
    const std::string path(raw.begin(), std::find(raw.begin(), raw.end(), u8(0)));
    u32 fd = 0, error = NV_SUCCESS;
    if (path == "/dev/nvmap" || path == "/dev/nvhost-ctrl") {
        fd = m_nextFd++;
        m_fds[fd] = path;
        Logger::Log(Logger::Level::Info, "[nvdrv] Open(\"" + path + "\") -> fd " + std::to_string(fd));
    } else {
        // Dispositivos de la GPU de verdad (nvhost-gpu, nvhost-as-gpu...): todavia no
        ctx.Unsupported("nvdrv: el dispositivo " + path + " no esta implementado");
        return;
    }
    ctx.Push<u32>(fd);
    ctx.Push<u32>(error);
    ctx.SetResult(Result::Success);
}

// Ioctl(fd, request, buffer de entrada, buffer de salida) -> u32 error
void NvDrv::Ioctl(IpcContext& ctx) {
    const u32 fd = ctx.Pop<u32>();
    const u32 request = ctx.Pop<u32>();
    auto it = m_fds.find(fd);
    if (it == m_fds.end()) {
        ctx.Push<u32>(NV_BAD_VALUE);
        ctx.SetResult(Result::Success);
        return;
    }
    // Los datos de entrada y salida tienen el tamano codificado en el request
    std::vector<u8> data = ctx.ReadBuffer(0);
    data.resize(std::max<size_t>(data.size(), IocSize(request)), 0);

    u32 error;
    if (it->second == "/dev/nvmap")            error = IoctlNvMap(ctx, request, data);
    else                                       error = IoctlNvHostCtrl(ctx, request, data);
    if (!ctx.IsHandled()) return;

    ctx.WriteBuffer(data.data(), data.size(), 0);
    ctx.Push<u32>(error);
    ctx.SetResult(Result::Success);
}

void NvDrv::Close(IpcContext& ctx) {
    m_fds.erase(ctx.Pop<u32>());
    ctx.Push<u32>(NV_SUCCESS);
    ctx.SetResult(Result::Success);
}

// ----------------------------------------------------------------------------
//  /dev/nvmap: bloques de memoria que la GPU puede usar
// ----------------------------------------------------------------------------
u32 NvDrv::IoctlNvMap(IpcContext& ctx, u32 request, std::vector<u8>& d) {
    NvMapTable& nvmap = ctx.GetKernel().GetDisplay().NvMap();
    switch (IocNr(request)) {
        case 0x01: { // CREATE(size) -> handle
            Put<u32>(d, 4, nvmap.Create(Get<u32>(d, 0)).handle);
            return NV_SUCCESS;
        }
        case 0x03: { // FROM_ID(id) -> handle
            const NvMapObject* obj = nvmap.FindById(Get<u32>(d, 0));
            if (!obj) return NV_BAD_VALUE;
            Put<u32>(d, 4, obj->handle);
            return NV_SUCCESS;
        }
        case 0x04: { // ALLOC(handle, heapmask, flags, align, kind, addr): asocia memoria del programa
            NvMapObject* obj = nvmap.FindByHandle(Get<u32>(d, 0));
            if (!obj) return NV_BAD_VALUE;
            obj->align = Get<u32>(d, 12);
            obj->kind = Get<u8>(d, 16);
            obj->address = Get<u64>(d, 24);
            return NV_SUCCESS;
        }
        case 0x05: { // FREE(handle) -> refcount, size, flags
            const u32 handle = Get<u32>(d, 0);
            const NvMapObject* obj = nvmap.FindByHandle(handle);
            if (!obj) return NV_BAD_VALUE;
            Put<u64>(d, 8, 0);
            Put<u32>(d, 16, obj->size);
            Put<u32>(d, 20, 0);
            nvmap.Free(handle);
            return NV_SUCCESS;
        }
        case 0x09: { // PARAM(handle, param) -> valor
            const NvMapObject* obj = nvmap.FindByHandle(Get<u32>(d, 0));
            if (!obj) return NV_BAD_VALUE;
            u32 value = 0;
            switch (Get<u32>(d, 4)) {
                case 1: value = obj->size;  break; // Size
                case 2: value = obj->align; break; // Alignment
                case 3: value = 0x40000000; break; // Base
                case 4: value = 0x40000000; break; // Heap
                case 5: value = obj->kind;  break; // Kind
                default: return NV_NOT_SUPPORTED;
            }
            Put<u32>(d, 8, value);
            return NV_SUCCESS;
        }
        case 0x0E: { // GET_ID(handle) -> id   (el id va primero en la estructura)
            const NvMapObject* obj = nvmap.FindByHandle(Get<u32>(d, 4));
            if (!obj) return NV_BAD_VALUE;
            Put<u32>(d, 0, obj->id);
            return NV_SUCCESS;
        }
        default:
            ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvmap no implementado");
            return NV_NOT_SUPPORTED;
    }
}

// ----------------------------------------------------------------------------
//  /dev/nvhost-ctrl: puntos de sincronizacion (syncpoints) y eventos
//  Sin GPU de verdad todo trabajo "ya ha terminado", asi que las esperas vuelven al momento.
// ----------------------------------------------------------------------------
u32 NvDrv::IoctlNvHostCtrl(IpcContext& ctx, u32 request, std::vector<u8>& d) {
    (void)d;
    if (IocType(request) != 0x00) {
        ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvhost-ctrl no implementado");
        return NV_NOT_SUPPORTED;
    }
    switch (IocNr(request)) {
        case 0x1C: // EVENT_SIGNAL
        case 0x1F: // EVENT_REGISTER
        case 0x20: // EVENT_UNREGISTER
        case 0x16: // SYNCPT_WAIT
        case 0x19: // SYNCPT_WAIT_EX
        case 0x1D: // EVENT_WAIT
        case 0x1E: // EVENT_WAIT_ASYNC
            return NV_SUCCESS;
        default:
            ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvhost-ctrl no implementado");
            return NV_NOT_SUPPORTED;
    }
}

} // namespace NeXo2::HLE
