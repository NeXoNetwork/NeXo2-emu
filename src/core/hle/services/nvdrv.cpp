#include "nvdrv.hpp"
#include "hle/kernel.hpp"
#include "video_core/gpu.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace NeXo2::HLE {

using Common::Logger;

namespace {
// Errores de NVIDIA (los devuelve el ioctl, no el resultado IPC)
constexpr u32 NV_SUCCESS       = 0;
constexpr u32 NV_NOT_SUPPORTED = 1;
constexpr u32 NV_BAD_VALUE     = 4;
constexpr u32 NV_TIMEOUT       = 5;
constexpr u32 NV_NO_MEMORY     = 0xA;

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
    RegisterCommand(1, "Ioctl", [this](IpcContext& ctx) { Ioctl(ctx, 1); });
    RegisterCommand(2, "Close", [this](IpcContext& ctx) { Close(ctx); });
    // Initialize(tamano de la memoria de transferencia, handle del proceso, handle de la memoria)
    RegisterStub(3, "Initialize");
    RegisterCommand(4, "QueryEvent", [this](IpcContext& ctx) { QueryEvent(ctx); });
    RegisterStub(8, "SetAruid");
    RegisterCommand(11, "Ioctl2", [this](IpcContext& ctx) { Ioctl(ctx, 2); });
    RegisterCommand(12, "Ioctl3", [this](IpcContext& ctx) { Ioctl(ctx, 3); });
    RegisterStub(13, "SetGraphicsFirmwareMemoryMarginEnabled");
}

// Open(ruta en buffer A) -> u32 fd, u32 error
void NvDrv::Open(IpcContext& ctx) {
    const std::vector<u8> raw = ctx.ReadBuffer(0);
    const std::string path(raw.begin(), std::find(raw.begin(), raw.end(), u8(0)));
    static const std::map<std::string, Device> devices = {
        {"/dev/nvmap", Device::NvMap},           {"/dev/nvhost-ctrl", Device::Ctrl},
        {"/dev/nvhost-ctrl-gpu", Device::CtrlGpu}, {"/dev/nvhost-as-gpu", Device::AsGpu},
        {"/dev/nvhost-gpu", Device::Gpu},
    };
    auto it = devices.find(path);
    if (it == devices.end()) {
        ctx.Unsupported("nvdrv: el dispositivo " + path + " no esta implementado");
        return;
    }
    const u32 fd = m_nextFd++;
    File file{it->second, path};
    if (file.device == Device::Gpu) file.channel = ctx.GetKernel().GetGpu().CreateChannel().Id();
    m_fds[fd] = file;
    Logger::Log(Logger::Level::Info, "[nvdrv] Open(\"" + path + "\") -> fd " + std::to_string(fd));
    ctx.Push<u32>(fd);
    ctx.Push<u32>(NV_SUCCESS);
    ctx.SetResult(Result::Success);
}

// Ioctl(fd, request, buffer de entrada, buffer de salida) -> u32 error
void NvDrv::Ioctl(IpcContext& ctx, int version) {
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
    std::vector<u8> extra;
    if (version == 2) extra = ctx.ReadBuffer(1);   // Ioctl2: datos extra de entrada (p. ej. entradas GPFIFO)

    File& file = it->second;
    u32 error = NV_NOT_SUPPORTED;
    switch (file.device) {
        case Device::NvMap:   error = IoctlNvMap(ctx, request, data); break;
        case Device::Ctrl:    error = IoctlCtrl(ctx, request, data); break;
        case Device::CtrlGpu: error = IoctlCtrlGpu(ctx, request, data); break;
        case Device::AsGpu:   error = IoctlAsGpu(ctx, request, data); break;
        case Device::Gpu:     error = IoctlGpu(ctx, file, request, data, extra); break;
    }
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

// QueryEvent(fd, id) -> handle del evento. Para nvhost-ctrl el id 0x1000000X es el
// evento X de EVENT_WAIT_ASYNC: se activa cuando el syncpoint llega a su valor.
void NvDrv::QueryEvent(IpcContext& ctx) {
    const u32 fd = ctx.Pop<u32>();
    const u32 id = ctx.Pop<u32>();
    Kernel& k = ctx.GetKernel();
    auto ev = std::make_shared<KEvent>("evento nvdrv");
    auto it = m_fds.find(fd);
    if (it != m_fds.end() && it->second.device == Device::Ctrl && (id >> 28) == 1)
        k.NvEventSlot(id & 0xFF) = ev;   // EVENT_WAIT_ASYNC lo activara
    ctx.PushCopyHandle(k.Handles().Create(ev));
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
            Put<u64>(d, 8, obj->address);
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
//  /dev/nvhost-ctrl: syncpoints y eventos. La GPU emulada termina el trabajo al
//  enviarlo, asi que casi siempre el syncpoint ya ha llegado.
// ----------------------------------------------------------------------------
u32 NvDrv::IoctlCtrl(IpcContext& ctx, u32 request, std::vector<u8>& d) {
    if (IocType(request) != 0x00) {
        ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvhost-ctrl no implementado");
        return NV_NOT_SUPPORTED;
    }
    Kernel& k = ctx.GetKernel();
    GPU::Syncpoints& sp = k.GetGpu().GetSyncpoints();
    switch (IocNr(request)) {
        case 0x14: // SYNCPT_READ(id) -> valor
        case 0x1A: // SYNCPT_READ_MAX(id) -> valor
            Put<u32>(d, 4, sp.Read(Get<u32>(d, 0)));
            return NV_SUCCESS;
        case 0x15: // SYNCPT_INCR(id)
            sp.Increment(Get<u32>(d, 0));
            return NV_SUCCESS;
        case 0x16: // SYNCPT_WAIT(id, umbral, timeout)
            return sp.Reached(Get<u32>(d, 0), Get<u32>(d, 4)) ? NV_SUCCESS : NV_TIMEOUT;
        case 0x19: // SYNCPT_WAIT_EX(id, umbral, timeout) -> valor
            Put<u32>(d, 12, sp.Read(Get<u32>(d, 0)));
            return sp.Reached(Get<u32>(d, 0), Get<u32>(d, 4)) ? NV_SUCCESS : NV_TIMEOUT;
        case 0x1D: // EVENT_WAIT(id, umbral, timeout, evento)
        case 0x1E: { // EVENT_WAIT_ASYNC(id, umbral, timeout, evento)
            const u32 id = Get<u32>(d, 0), threshold = Get<u32>(d, 4), slot = Get<u32>(d, 12) & 0xFF;
            if (sp.Reached(id, threshold)) return NV_SUCCESS;
            // Aun no: el programa esperara al evento; se activa cuando el syncpoint llegue
            std::weak_ptr<KEvent> ev = k.NvEventSlot(slot);
            sp.AddWaiter(id, threshold, [ev] { if (auto e = ev.lock()) e->signaled = true; });
            return NV_TIMEOUT;
        }
        case 0x1C: // EVENT_SIGNAL(evento): cancelar una espera
            if (auto& e = k.NvEventSlot(Get<u32>(d, 0) & 0xFF)) e->signaled = true;
            return NV_SUCCESS;
        case 0x1F: // EVENT_REGISTER
        case 0x20: // EVENT_UNREGISTER
        case 0x21: // EVENT_KILL
            return NV_SUCCESS;
        default:
            ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvhost-ctrl no implementado");
            return NV_NOT_SUPPORTED;
    }
}

// ----------------------------------------------------------------------------
//  /dev/nvhost-ctrl-gpu: informacion de la GPU (la de la Switch 1: GM20B)
// ----------------------------------------------------------------------------
u32 NvDrv::IoctlCtrlGpu(IpcContext& ctx, u32 request, std::vector<u8>& d) {
    switch (IocNr(request)) {
        case 0x01: // ZCULL_GET_CTX_SIZE
            Put<u32>(d, 0, 0x8000);
            return NV_SUCCESS;
        case 0x02: { // ZCULL_GET_INFO
            const u32 info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
            for (int i = 0; i < 10; ++i) Put<u32>(d, size_t(i) * 4, info[i]);
            return NV_SUCCESS;
        }
        case 0x03: // ZBC_SET_TABLE: valores de borrado rapido (compresion); no hace falta
        case 0x04: // ZBC_QUERY_TABLE
            return NV_SUCCESS;
        case 0x05: { // GET_CHARACTERISTICS(tamano, direccion) -> estructura de 0xA0 bytes
            Put<u64>(d, 0, 0xA0);
            const size_t o = 16;
            Put<u32>(d, o + 0, 0x120);          // arquitectura GM200
            Put<u32>(d, o + 4, 0xB);            // GM20B
            Put<u32>(d, o + 8, 0xA1);           // revision A1
            Put<u32>(d, o + 12, 1);             // GPCs
            Put<u64>(d, o + 16, 0x40000);       // cache L2
            Put<u64>(d, o + 24, 0);
            Put<u32>(d, o + 32, 2);             // TPCs por GPC
            Put<u32>(d, o + 36, 0x20);          // bus AXI
            Put<u32>(d, o + 40, 0x20000);       // pagina grande
            Put<u32>(d, o + 44, 0x20000);
            Put<u32>(d, o + 48, 0x1B);
            Put<u32>(d, o + 52, 0x30000);
            Put<u32>(d, o + 56, 1);             // mascara de GPCs
            Put<u32>(d, o + 60, 0x503);         // SM 5.3
            Put<u32>(d, o + 64, 0x503);
            Put<u32>(d, o + 68, 0x80);          // warps
            Put<u32>(d, o + 72, 0x28);          // 40 bits de direccion
            Put<u32>(d, o + 76, 0);
            Put<u64>(d, o + 80, 0x55);
            Put<u32>(d, o + 88, 0x902D);        // clases: 2D, 3D, compute, GPFIFO, inline, DMA
            Put<u32>(d, o + 92, 0xB197);
            Put<u32>(d, o + 96, 0xB1C0);
            Put<u32>(d, o + 100, 0xB06F);
            Put<u32>(d, o + 104, 0xA140);
            Put<u32>(d, o + 108, 0xB0B5);
            Put<u32>(d, o + 112, 1);
            Put<u32>(d, o + 116, 0);
            Put<u32>(d, o + 120, 2);
            Put<u32>(d, o + 124, 1);
            Put<u32>(d, o + 128, 0);
            Put<u32>(d, o + 132, 1);
            Put<u32>(d, o + 136, 0x21D70);
            Put<u32>(d, o + 140, 0);
            Put<u64>(d, o + 144, 0x6230326D67ull);   // "gm20b"
            Put<u64>(d, o + 152, 0);
            return NV_SUCCESS;
        }
        case 0x06: // GET_TPC_MASKS -> mascara de TPCs (2 TPCs)
            Put<u32>(d, 16, 0x3);
            return NV_SUCCESS;
        case 0x14: // ZBC_GET_ACTIVE_SLOT_MASK
            Put<u32>(d, 0, 0x7);
            Put<u32>(d, 4, 0);
            return NV_SUCCESS;
        case 0x1C: { // GET_GPU_TIME (ns)
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            Put<u64>(d, 0, u64(ns));
            return NV_SUCCESS;
        }
        default:
            ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvhost-ctrl-gpu no implementado");
            return NV_NOT_SUPPORTED;
    }
}

// ----------------------------------------------------------------------------
//  /dev/nvhost-as-gpu: direcciones virtuales de la GPU
// ----------------------------------------------------------------------------
u32 NvDrv::IoctlAsGpu(IpcContext& ctx, u32 request, std::vector<u8>& d) {
    GPU::GpuMemoryManager& mm = ctx.GetKernel().GetGpu().MemoryManager();
    using MM = GPU::GpuMemoryManager;
    switch (IocNr(request)) {
        case 0x01: // BIND_CHANNEL(fd)
        case 0x09: // INITIALIZE_EX
            return NV_SUCCESS;
        case 0x02: { // ALLOC_SPACE(paginas, tamano pagina, flags, alineacion/direccion)
            const u64 page = Get<u32>(d, 4);
            const u64 size = u64(Get<u32>(d, 0)) * page;
            const u32 flags = Get<u32>(d, 8);
            if (flags & 1) {   // direccion fija
                mm.ReserveFixed(Get<u64>(d, 16), size);
                return NV_SUCCESS;
            }
            const u64 va = mm.Allocate(size, std::max<u64>(Get<u64>(d, 16), page), page >= MM::BIG_PAGE);
            if (!va) return NV_NO_MEMORY;
            Put<u64>(d, 16, va);
            return NV_SUCCESS;
        }
        case 0x03: // FREE_SPACE(direccion, paginas, tamano pagina)
            mm.Free(Get<u64>(d, 0), u64(Get<u32>(d, 8)) * Get<u32>(d, 12));
            return NV_SUCCESS;
        case 0x05: // UNMAP_BUFFER(direccion)
            mm.Unmap(Get<u64>(d, 0));
            return NV_SUCCESS;
        case 0x06: { // MAP_BUFFER_EX(flags, kind, handle nvmap, tamano pagina, desplazamiento, tamano, direccion)
            const u32 flags = Get<u32>(d, 0);
            if (flags & 0x100) return NV_SUCCESS;   // MODIFY: solo cambia el "kind" (compresion)
            const NvMapObject* obj = ctx.GetKernel().GetDisplay().NvMap().FindByHandle(Get<u32>(d, 8));
            if (!obj || !obj->address) return NV_BAD_VALUE;
            u32 page = Get<u32>(d, 12);
            if (page == 0) page = u32(MM::BIG_PAGE);
            const u64 offset = Get<u64>(d, 16);
            u64 size = Get<u64>(d, 24);
            if (size == 0) size = obj->size > offset ? obj->size - offset : 0;
            u64 va = Get<u64>(d, 32);
            if (!(flags & 1)) {   // sin direccion fija: buscar una
                va = mm.Allocate(size, page, page >= MM::BIG_PAGE);
                if (!va) return NV_NO_MEMORY;
            }
            mm.Map(va, obj->address + offset, size);
            Put<u32>(d, 12, page);
            Put<u64>(d, 32, va);
            return NV_SUCCESS;
        }
        case 0x08: { // GET_VA_REGIONS -> dos regiones (paginas de 4 KB y de 64 KB)
            Put<u32>(d, 8, 48);
            Put<u64>(d, 16, MM::SMALL_REGION_START);
            Put<u32>(d, 24, u32(MM::SMALL_PAGE));
            Put<u64>(d, 32, (MM::SMALL_REGION_END - MM::SMALL_REGION_START) / MM::SMALL_PAGE);
            Put<u64>(d, 40, MM::BIG_REGION_START);
            Put<u32>(d, 48, u32(MM::BIG_PAGE));
            Put<u64>(d, 56, (MM::BIG_REGION_END - MM::BIG_REGION_START) / MM::BIG_PAGE);
            return NV_SUCCESS;
        }
        default:
            ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvhost-as-gpu no implementado");
            return NV_NOT_SUPPORTED;
    }
}

// ----------------------------------------------------------------------------
//  /dev/nvhost-gpu: canal de comandos
// ----------------------------------------------------------------------------
u32 NvDrv::IoctlGpu(IpcContext& ctx, File& file, u32 request, std::vector<u8>& d, const std::vector<u8>& extra) {
    GPU::Gpu& gpu = ctx.GetKernel().GetGpu();
    GPU::Channel* ch = gpu.GetChannel(file.channel);
    if (!ch) return NV_BAD_VALUE;
    const u32 type = IocType(request), nr = IocNr(request);

    // Fence que devuelve el canal: su syncpoint y el valor actual (el trabajo ya esta hecho)
    auto put_fence = [&](size_t off) {
        Put<u32>(d, off, ch->SyncpointId());
        Put<u32>(d, off + 4, gpu.GetSyncpoints().Read(ch->SyncpointId()));
    };

    if (type == 0x48) {
        switch (nr) {
            case 0x01: // SET_NVMAP_FD
            case 0x03: // SET_TIMEOUT
            case 0x0B: // ZCULL_BIND
            case 0x0C: // SET_ERROR_NOTIFIER
            case 0x0D: // SET_PRIORITY
                return NV_SUCCESS;
            case 0x08: { // SUBMIT_GPFIFO(entradas en el mismo buffer, a partir del byte 24)
                const u32 count = Get<u32>(d, 8);
                std::vector<u64> entries(count);
                for (u32 i = 0; i < count; ++i) entries[i] = Get<u64>(d, 24 + size_t(i) * 8);
                ch->SubmitGpfifo(entries);
                put_fence(16);
                return NV_SUCCESS;
            }
            case 0x1B: { // KICKOFF_PB (Ioctl2: entradas en el buffer extra)
                const u32 count = Get<u32>(d, 8);
                std::vector<u64> entries(count);
                for (u32 i = 0; i < count && (size_t(i) + 1) * 8 <= extra.size(); ++i)
                    std::memcpy(&entries[i], extra.data() + size_t(i) * 8, 8);
                ch->SubmitGpfifo(entries);
                put_fence(16);
                return NV_SUCCESS;
            }
            case 0x09: // ALLOC_OBJ_CTX(clase, flags) -> id
                Put<u64>(d, 8, Get<u32>(d, 0));
                return NV_SUCCESS;
            case 0x1A: // ALLOC_GPFIFO_EX2 -> fence inicial
                put_fence(12);
                return NV_SUCCESS;
            case 0x16: // GET_ERROR_INFO: ningun error
            case 0x17: // GET_ERROR_NOTIFICATION
                std::fill(d.begin(), d.end(), u8(0));
                return NV_SUCCESS;
            default: break;
        }
    } else if (type == 0x47 && nr == 0x14) {   // SET_USER_DATA
        return NV_SUCCESS;
    } else if (type == 0x00 && (nr == 0x02 || nr == 0x07 || nr == 0x08 || nr == 0x14 || nr == 0x23)) {
        // GET_SYNCPT, SET_SUBMIT_TIMEOUT, SET_CLK_RATE, GET_CLK_RATE...
        if (nr == 0x02) Put<u32>(d, 4, ch->SyncpointId());
        return NV_SUCCESS;
    }
    ctx.Unsupported("nvdrv: ioctl " + Hex(request) + " de /dev/nvhost-gpu no implementado");
    return NV_NOT_SUPPORTED;
}

} // namespace NeXo2::HLE
