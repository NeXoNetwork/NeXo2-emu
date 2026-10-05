#include "vi.hpp"
#include "hle/kernel.hpp"
#include "common/logger.hpp"
#include <cstring>

namespace NeXo2::HLE {

using Common::Logger;

namespace {
// Codigos del protocolo IGraphicBufferProducer (version que usa la Switch)
namespace Code {
    constexpr u32 RequestBuffer = 1, SetBufferCount = 2, DequeueBuffer = 3, DetachBuffer = 4,
                  QueueBuffer = 7, CancelBuffer = 8, Query = 9, Connect = 10, Disconnect = 11,
                  SetPreallocatedBuffer = 14;
}
constexpr s32 STATUS_OK = 0;
constexpr s32 STATUS_WOULD_BLOCK = -11;
constexpr s32 STATUS_BAD_VALUE = -22;

// BqBufferOutput: lo que devuelven CONNECT y QUEUE_BUFFER
struct BufferOutput { u32 width, height, transform_hint, num_pending_buffers; };

template <typename T> T Word(const std::vector<u8>& d, size_t index) {
    T v{};
    if ((index + 1) * 4 <= d.size()) std::memcpy(&v, d.data() + index * 4, sizeof(T) <= 4 ? sizeof(T) : 4);
    return v;
}
} // namespace

// ============================================================================
//  Parcels
// ============================================================================

ParcelReader::ParcelReader(const std::vector<u8>& raw) {
    // Cabecera: payload_size, payload_off, objects_size, objects_off
    if (raw.size() < 16) { m_ok = false; return; }
    u32 size, off;
    std::memcpy(&size, raw.data(), 4);
    std::memcpy(&off, raw.data() + 4, 4);
    if (u64(off) + size > raw.size()) { m_ok = false; return; }
    m_payload.assign(raw.begin() + off, raw.begin() + off + size);
}

s32 ParcelReader::ReadI32() {
    s32 v = 0;
    if (m_pos + 4 > m_payload.size()) { m_ok = false; return 0; }
    std::memcpy(&v, m_payload.data() + m_pos, 4);
    m_pos += 4;
    return v;
}

void ParcelReader::ReadInterfaceToken() {
    ReadI32();                                   // politica estricta (0x100)
    const s32 len = ReadI32();                   // String16: longitud + caracteres de 16 bits + 0
    if (len < 0) return;
    m_pos += ((size_t(len) + 1) * 2 + 3) & ~size_t(3);
}

std::vector<u8> ParcelReader::ReadFlattened() {
    const s32 len = ReadI32();
    const s32 fds = ReadI32();
    if (len < 0 || fds != 0 || m_pos + size_t(len) > m_payload.size()) { m_ok = false; return {}; }
    std::vector<u8> out(m_payload.begin() + m_pos, m_payload.begin() + m_pos + len);
    m_pos += (size_t(len) + 3) & ~size_t(3);
    return out;
}

void ParcelWriter::WriteI32(s32 v) { WriteBytes(&v, 4); }

void ParcelWriter::WriteBytes(const void* data, size_t size) {
    const u8* p = static_cast<const u8*>(data);
    m_payload.insert(m_payload.end(), p, p + size);
    while (m_payload.size() % 4) m_payload.push_back(0);
}

void ParcelWriter::WriteFlattened(const void* data, size_t size) {
    WriteI32(static_cast<s32>(size));
    WriteI32(0); // sin descriptores de archivo
    WriteBytes(data, size);
}

std::vector<u8> ParcelWriter::Serialize() const {
    const u32 header[4] = {u32(m_payload.size()), 16, 0, u32(16 + m_payload.size())};
    std::vector<u8> out(16 + m_payload.size());
    std::memcpy(out.data(), header, 16);
    if (!m_payload.empty()) std::memcpy(out.data() + 16, m_payload.data(), m_payload.size());
    return out;
}

// GraphicBuffer serializado: magic, width, height, stride, format, usage, pid, refcount,
// numFds, numInts y luego los "ints": el NvGraphicBuffer de NVIDIA sin sus 8 bytes de cabecera.
// La palabra k del NvGraphicBuffer (desplazamiento O) esta en la palabra 10 + (O - 8) / 4.
bool ParseGraphicBuffer(const std::vector<u8>& flat, GraphicBufferInfo& out) {
    // Palabras (u32) del GraphicBuffer serializado:
    //   0..9   cabecera de Android: "GBFR", ancho, alto, stride, formato, uso, pid, refs, nFds, nInts
    //   10..   NvGraphicBuffer: -1, nvmap_id, 0, 0xDAFFCAFF, pid, tipo, uso, formato, formato_ext,
    //          stride, tamano_total, n_planos, 0, y en la palabra 23 el plano 0 (NvSurface):
    //          +0 ancho, +1 alto, +2/+3 color (u64), +4 layout, +5 pitch, +6 sin uso, +7 offset,
    //          +8 kind, +9 block_height_log2, +10 scan, +11 offset 2o campo, +12/+13 flags, +14/+15 tamano
    if (flat.size() < 39 * 4 || Word<u32>(flat, 0) != 0x47424652) return false; // "GBFR"
    constexpr u32 PLANE0 = 23;
    out.nvmap_id          = Word<u32>(flat, 11);
    out.format            = Word<u32>(flat, 17);
    out.width             = Word<u32>(flat, PLANE0 + 0);
    out.height            = Word<u32>(flat, PLANE0 + 1);
    out.layout            = Word<u32>(flat, PLANE0 + 4);
    out.pitch             = Word<u32>(flat, PLANE0 + 5);
    out.offset            = Word<u32>(flat, PLANE0 + 7);
    out.block_height_log2 = Word<u32>(flat, PLANE0 + 9);
    out.size              = Word<u32>(flat, PLANE0 + 14);
    return out.width != 0 && out.height != 0;
}

// ============================================================================
//  vi:m / vi:s / vi:u
// ============================================================================

ViRoot::ViRoot(std::string name, u32 get_display_service_cmd) : ServiceObject(std::move(name)) {
    // GetDisplayService(u32 politica) -> IApplicationDisplayService
    RegisterCommand(get_display_service_cmd, "GetDisplayService", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<ApplicationDisplayService>(std::make_shared<ViState>()));
        ctx.SetResult(Result::Success);
    });
}

ApplicationDisplayService::ApplicationDisplayService(std::shared_ptr<ViState> state)
    : ServiceObject("IApplicationDisplayService"), m_state(std::move(state)) {
    auto st = m_state;
    RegisterCommand(100, "GetRelayService", [st](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<HosBinderDriver>(st));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(101, "GetSystemDisplayService", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<SystemDisplayService>());
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(102, "GetManagerDisplayService", [st](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<ManagerDisplayService>(st));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(103, "GetIndirectDisplayTransactionService", [st](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<HosBinderDriver>(st));
        ctx.SetResult(Result::Success);
    });
    // OpenDisplay(nombre de 0x40 bytes) -> id de la pantalla. Solo hay una: "Default".
    RegisterCommand(1010, "OpenDisplay", [](IpcContext& ctx) {
        ctx.Push<u64>(0);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1011, "OpenDefaultDisplay", [](IpcContext& ctx) {
        ctx.Push<u64>(0);
        ctx.SetResult(Result::Success);
    });
    RegisterStub(1020, "CloseDisplay");
    RegisterStub(1101, "SetDisplayEnabled");
    RegisterCommand(1102, "GetDisplayResolution", [](IpcContext& ctx) {
        ctx.Push<s64>(Display::WIDTH);
        ctx.Push<s64>(Display::HEIGHT);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(2020, "OpenLayer", [this](IpcContext& ctx) { OpenLayer(ctx); });
    RegisterStub(2021, "CloseLayer");
    RegisterCommand(2030, "CreateStrayLayer", [this](IpcContext& ctx) { CreateStrayLayer(ctx); });
    RegisterStub(2031, "DestroyStrayLayer");
    RegisterStub(2101, "SetLayerScalingMode");
    // GetDisplayVsyncEvent: evento siempre activo (no limitamos a 60 Hz todavia)
    RegisterCommand(5202, "GetDisplayVsyncEvent", [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent("vsync", true));
        ctx.SetResult(Result::Success);
    });
}

u64 ApplicationDisplayService::WriteNativeWindow(IpcContext& ctx, u64 layer_id) {
    // El id del binder es el mismo que el de la capa; su cola se crea aqui
    const u32 binder_id = static_cast<u32>(layer_id);
    m_state->queues[binder_id];

    // Parcel con la "ventana nativa": libnx solo lee payload[2] = id del binder
    struct NativeWindow {
        u32 magic = 2;
        u32 process_id = 1;
        u32 binder_id = 0;
        u32 reserved[3] = {};
        char driver[8] = {'d', 'i', 's', 'p', 'd', 'r', 'v', '\0'};
        u64 reserved2 = 0;
    } window;
    window.binder_id = binder_id;
    ParcelWriter w;
    w.WriteBytes(&window, sizeof(window));
    const std::vector<u8> parcel = w.Serialize();
    ctx.WriteBuffer(parcel.data(), parcel.size(), 0);
    return parcel.size();
}

// OpenLayer(nombre de pantalla, id de capa, aruid, pid) -> u64 tamano + ventana nativa
void ApplicationDisplayService::OpenLayer(IpcContext& ctx) {
    ctx.SkipBytes(0x40);                 // nombre de la pantalla
    const u64 layer_id = ctx.Pop<u64>();
    Logger::Log(Logger::Level::Info, "[vi] OpenLayer(" + std::to_string(layer_id) + ")");
    ctx.Push<u64>(WriteNativeWindow(ctx, layer_id));
    ctx.SetResult(Result::Success);
}

// CreateStrayLayer(flags, id de pantalla) -> id de capa, tamano + ventana nativa
void ApplicationDisplayService::CreateStrayLayer(IpcContext& ctx) {
    const u64 layer_id = m_state->next_layer_id++;
    ctx.Push<u64>(layer_id);
    ctx.Push<u64>(WriteNativeWindow(ctx, layer_id));
    ctx.SetResult(Result::Success);
}

SystemDisplayService::SystemDisplayService() : ServiceObject("ISystemDisplayService") {
    RegisterStub(2201, "SetLayerPosition");
    RegisterStub(2203, "SetLayerSize");
    RegisterStub(2205, "SetLayerZ");
    RegisterStub(2207, "SetLayerVisibility");
    RegisterCommand(1203, "GetDisplayLogicalResolution", [](IpcContext& ctx) {
        ctx.Push<s32>(Display::WIDTH);
        ctx.Push<s32>(Display::HEIGHT);
        ctx.SetResult(Result::Success);
    });
}

ManagerDisplayService::ManagerDisplayService(std::shared_ptr<ViState> state)
    : ServiceObject("IManagerDisplayService"), m_state(std::move(state)) {
    auto st = m_state;
    RegisterCommand(2010, "CreateManagedLayer", [st](IpcContext& ctx) {
        ctx.Push<u64>(st->next_layer_id++);
        ctx.SetResult(Result::Success);
    });
    RegisterStub(2011, "DestroyManagedLayer");
    RegisterStub(6000, "AddToLayerStack");
    RegisterStub(6002, "SetLayerVisibility");
    RegisterStub(7000, "SetContentVisibility");
}

// ============================================================================
//  IHOSBinderDriverRelay: el canal de la cola de buffers
// ============================================================================

HosBinderDriver::HosBinderDriver(std::shared_ptr<ViState> state)
    : ServiceObject("IHOSBinderDriverRelay"), m_state(std::move(state)) {
    RegisterCommand(0, "TransactParcel",     [this](IpcContext& ctx) { TransactParcel(ctx); });
    RegisterCommand(3, "TransactParcelAuto", [this](IpcContext& ctx) { TransactParcel(ctx); });
    RegisterStub(1, "AdjustRefcount");
    // GetNativeHandle: evento "hay un buffer libre". Siempre activo: nunca hacemos esperar.
    RegisterCommand(2, "GetNativeHandle", [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent("buffer libre", true));
        ctx.SetResult(Result::Success);
    });
}

// TransactParcel(id del binder, codigo, flags, parcel de entrada) -> parcel de respuesta
void HosBinderDriver::TransactParcel(IpcContext& ctx) {
    const u32 binder_id = static_cast<u32>(ctx.Pop<s32>());
    const u32 code = ctx.Pop<u32>();
    ctx.SkipBytes(4); // flags
    auto it = m_state->queues.find(binder_id);
    if (it == m_state->queues.end()) {
        ctx.Unsupported("vi: binder " + std::to_string(binder_id) + " desconocido");
        return;
    }
    const std::vector<u8> reply = Transact(ctx, it->second, code, ctx.ReadBuffer(0));
    if (!ctx.IsHandled()) return;
    ctx.WriteBuffer(reply.data(), reply.size(), 0);
    ctx.SetResult(Result::Success);
}

std::vector<u8> HosBinderDriver::Transact(IpcContext& ctx, BufferQueue& q, u32 code, const std::vector<u8>& in) {
    ParcelReader r(in);
    ParcelWriter w;
    r.ReadInterfaceToken();
    const BufferOutput output{Display::WIDTH, Display::HEIGHT, 0, 0};

    switch (code) {
        case Code::Connect:
            q.connected = true;
            w.WriteBytes(&output, sizeof(output));
            w.WriteI32(STATUS_OK);
            break;

        case Code::Disconnect:
            q.connected = false;
            for (auto& s : q.slots) s.dequeued = false;
            w.WriteI32(STATUS_OK);
            break;

        case Code::SetPreallocatedBuffer: {
            const s32 slot = r.ReadI32();
            const bool has_input = r.ReadI32() != 0;
            if (slot < 0 || slot >= 64) { w.WriteI32(STATUS_BAD_VALUE); break; }
            auto& s = q.slots[slot];
            s = BufferQueue::Slot{};
            if (has_input) {
                s.configured = ParseGraphicBuffer(r.ReadFlattened(), s.info);
                Logger::Log(Logger::Level::Info, "[vi] Buffer " + std::to_string(slot) + ": " +
                    std::to_string(s.info.width) + "x" + std::to_string(s.info.height) +
                    " formato " + std::to_string(s.info.format) + (s.info.layout == 3 ? " (block linear)" : " (lineal)"));
            }
            w.WriteI32(STATUS_OK);
            break;
        }

        case Code::DequeueBuffer: {
            // Entrega el siguiente buffer configurado y libre (por turnos)
            s32 found = -1;
            for (u32 k = 0; k < 64 && found < 0; ++k) {
                const u32 i = (q.next + k) % 64;
                if (q.slots[i].configured && !q.slots[i].dequeued) found = static_cast<s32>(i);
            }
            if (found < 0) { w.WriteI32(-1); w.WriteI32(0); w.WriteI32(STATUS_WOULD_BLOCK); break; }
            q.slots[found].dequeued = true;
            q.next = (found + 1) % 64;
            const u8 no_fences[36] = {}; // NvMultiFence vacio: el buffer ya esta listo
            w.WriteI32(found);
            w.WriteI32(1);
            w.WriteFlattened(no_fences, sizeof(no_fences));
            w.WriteI32(STATUS_OK);
            break;
        }

        case Code::RequestBuffer:
            r.ReadI32();          // hueco
            w.WriteI32(0);        // no devolvemos el GraphicBuffer (libnx no lo necesita)
            w.WriteI32(STATUS_OK);
            break;

        case Code::QueueBuffer: {
            const s32 slot = r.ReadI32();
            if (slot >= 0 && slot < 64 && q.slots[slot].configured) {
                q.slots[slot].dequeued = false;
                Display& display = ctx.GetKernel().GetDisplay();
                const bool first = display.Frame().count == 0;
                if (display.Present(ctx.GetMemory(), q.slots[slot].info) && first)
                    Logger::Log(Logger::Level::Info, "[vi] Primera imagen en pantalla (" +
                        std::to_string(display.Frame().width) + "x" + std::to_string(display.Frame().height) + ")");
            }
            w.WriteBytes(&output, sizeof(output));
            w.WriteI32(STATUS_OK);
            break;
        }

        case Code::CancelBuffer: {
            const s32 slot = r.ReadI32();
            if (slot >= 0 && slot < 64) q.slots[slot].dequeued = false;
            w.WriteI32(STATUS_OK);
            break;
        }

        case Code::Query: {
            const s32 what = r.ReadI32();
            s32 value = 0;
            switch (what) {
                case 0: value = Display::WIDTH;  break; // NATIVE_WINDOW_WIDTH
                case 1: value = Display::HEIGHT; break; // NATIVE_WINDOW_HEIGHT
                case 2: value = 1;               break; // NATIVE_WINDOW_FORMAT (RGBA_8888)
                case 3: value = 1;               break; // MIN_UNDEQUEUED_BUFFERS
                default: break;
            }
            w.WriteI32(value);
            w.WriteI32(STATUS_OK);
            break;
        }

        case Code::SetBufferCount:
        case Code::DetachBuffer:
            w.WriteI32(STATUS_OK);
            break;

        default:
            ctx.Unsupported("vi: codigo de binder " + std::to_string(code) + " no implementado");
            return {};
    }
    return w.Serialize();
}

} // namespace NeXo2::HLE
