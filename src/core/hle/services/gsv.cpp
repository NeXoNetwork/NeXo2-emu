#include "gsv.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <set>
#include "common/logger.hpp"
#include "hle/kernel.hpp"
#include "hle/kernel_objects.hpp"
#include "nvdrv.hpp"
#include "video_core/gpu.hpp"

namespace NeXo2::HLE {

namespace {

using Logger = Common::Logger;

std::string Hex(u64 v) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// Palabra 'i' de los datos de la peticion (TIPC: los argumentos empiezan directamente)
u32 Word(IpcContext& ctx, u32 i) {
    if (u64(i) * 4 + 4 > ctx.Request().payload_size) return 0;
    return ctx.GetMemory().Read<u32>(ctx.Request().payload_offset + u64(i) * 4);
}
u64 Pair(IpcContext& ctx, u32 i) { return u64(Word(ctx, i)) | (u64(Word(ctx, i + 1)) << 32); }

// Respuesta: resultado OK y estas palabras
void Reply(IpcContext& ctx, std::initializer_list<u32> words) {
    for (u32 w : words) ctx.Push<u32>(w);
    ctx.SetResult(0);
}
void ReplyU64(IpcContext& ctx, std::initializer_list<u64> values) {
    for (u64 v : values) ctx.Push<u64>(v);
    ctx.SetResult(0);
}

// Devuelve una sesion nueva como handle "copy" (lo que esperan estas interfaces aunque la
// pregunta venga de un dominio CMIF, como ISelfController)
void ReplySession(IpcContext& ctx, std::shared_ptr<ServiceObject> object) {
    ctx.PushCopyHandle(ctx.GetKernel().CreateSessionHandle(std::move(object)));
    ctx.SetResult(0);
}

// Base de las sesiones del sistema grafico: hablan TIPC y lo desconocido contesta OK con
// 16 palabras a cero (si se dejaran las de la peticion, el juego leeria basura como respuesta).
class GsvObject : public ServiceObject {
public:
    using ServiceObject::ServiceObject;
    bool SupportsTipc() const override { return true; }
    void Dispatch(IpcContext& ctx) override {
        if (HasCommand(ctx.CommandId())) { ServiceObject::Dispatch(ctx); return; }
        static std::set<std::string> warned;
        static const bool all = std::getenv("NEXO2_LOG_IPC") != nullptr;
        if (warned.insert(Name() + ":" + std::to_string(ctx.CommandId())).second || all) {
            std::string args;
            for (u32 i = 0; i < 6 && i * 4 < ctx.Request().payload_size; ++i) args += " " + Hex(Word(ctx, i));
            Logger::Log(Logger::Level::Warning, "[gsv] " + Name() + ": comando " + std::to_string(ctx.CommandId()) +
                        " desconocido (datos:" + args + "); OK con ceros");
        }
        for (int i = 0; i < 16; ++i) ctx.Push<u32>(0);
        ctx.SetResult(0);
    }
protected:
    // 0(u64): saludo de los SDK nuevos; hay que devolver el mismo numero
    void RegisterHandshake() {
        RegisterCommand(0, "Handshake", [](IpcContext& ctx) { Reply(ctx, {Word(ctx, 0), Word(ctx, 1)}); });
    }
    // Comando que contesta estas palabras
    void RegisterWords(u32 id, const char* name, std::vector<u32> words) {
        RegisterCommand(id, name, [words](IpcContext& ctx) {
            for (u32 w : words) ctx.Push<u32>(w);
            ctx.SetResult(0);
        });
    }
};

// Evento que el juego puede esperar; se activa poniendo 'signaled'
std::shared_ptr<KEvent> NewEvent(const std::string& name, bool signaled = false) {
    return std::make_shared<KEvent>(name, signaled);
}

// ---------------------------------------------------------------------------------------------
// syncpt (gsv 202): esperar a que un syncpoint de la GPU llegue a un valor.
// El juego pide un evento por "slot" (22), pide que se active cuando un contador llegue a un
// valor (201) o lo cancela (202). El contador es el numero del syncpoint con los bits al reves.
// ---------------------------------------------------------------------------------------------
class SyncptSession final : public GsvObject {
public:
    SyncptSession() : GsvObject("gsv:syncpt") {
        RegisterHandshake();
        // Cuantos syncpoints puede usar (el SDK exige < 0x400)
        RegisterWords(1, "GetCount", {32});
        RegisterCommand(22, "GetSlotEvent", [this](IpcContext& ctx) {
            ctx.PushCopyHandle(ctx.GetKernel().Handles().Create(Slot(Word(ctx, 0)).event));
            ctx.SetResult(0);
        });
        RegisterCommand(103, "QueryCounter", [](IpcContext& ctx) {
            const u32 id = ~Word(ctx, 0);
            const u64 value = ctx.GetKernel().GetGpu().GetSyncpoints().Read(id);
            Reply(ctx, {u32(value), u32(value >> 32), 0});
        });
        RegisterCommand(201, "RequestSlotEvent", [this](IpcContext& ctx) {
            const u32 id = ~Word(ctx, 1);
            const u32 value = Word(ctx, 2);
            static const bool log = std::getenv("NEXO2_GPU_WAIT_LOG") != nullptr;
            if (log) Logger::Log(Logger::Level::Debug, "[gsv] syncpt 201: slot " + std::to_string(Word(ctx, 0)) + " espera syncpoint " +
                                 std::to_string(id) + " >= " + Hex(value) + " (ahora " +
                                 Hex(ctx.GetKernel().GetGpu().GetSyncpoints().Read(id)) + ")");
            auto& slot = Slot(Word(ctx, 0));
            slot.event->signaled = false;   // un slot se reutiliza: empieza sin activar
            const u64 generation = ++slot.generation;
            if (id >= GPU::Syncpoints::COUNT) {
                slot.event->signaled = true;
            } else {
                std::weak_ptr<SlotState> weak = slot.self;
                ctx.GetKernel().GetGpu().GetSyncpoints().AddWaiter(id, value, [weak, generation] {
                    if (auto s = weak.lock(); s && s->generation == generation) s->event->signaled = true;
                });
            }
            Reply(ctx, {});
        });
        // Cancelar: el SDK espera el evento despues, asi que se activa siempre
        RegisterCommand(202, "CancelSlotEvent", [this](IpcContext& ctx) {
            auto& slot = Slot(Word(ctx, 0));
            ++slot.generation;
            slot.event->signaled = true;
            Reply(ctx, {});
        });
    }
private:
    struct SlotState {
        std::shared_ptr<KEvent> event;
        u64 generation = 0;
        std::shared_ptr<SlotState> self;   // para los avisos (weak_ptr)
    };
    SlotState& Slot(u32 slot) {
        auto& s = m_slots[slot];
        if (!s) {
            s = std::make_shared<SlotState>();
            s->event = NewEvent("syncpt slot " + std::to_string(slot));
            s->self = s;
        }
        return *s;
    }
    std::map<u32, std::shared_ptr<SlotState>> m_slots;
public:
    ~SyncptSession() override { for (auto& [id, s] : m_slots) s->self.reset(); }
};

// ---------------------------------------------------------------------------------------------
// gpu (gsv 497): memoria para la GPU y espacios de direcciones.
// Las operaciones terminan activando el evento de un id (los da 12; 21 "empieza" una).
// La sesion que da 22 es la de la memoria: 101 crea memoria accesible por la GPU (la GPU la usa en
// la misma direccion que la CPU), 211/213 la mapean y desmapean, 214 crea regiones solo de GPU.
// ---------------------------------------------------------------------------------------------
struct GpuState {
    static constexpr u32 NUM_IDS = 32;
    std::array<std::shared_ptr<KEvent>, NUM_IDS> events{};
    u32 next_memory = 1;
    struct GpuMemory { u64 address, size; };
    std::map<u32, GpuMemory> memories;   // handle -> memoria del proceso
    std::shared_ptr<KEvent> Event(u32 id) {
        auto& ev = events[id % NUM_IDS];
        if (!ev) ev = NewEvent("gpu id " + std::to_string(id % NUM_IDS));
        return ev;
    }
};

class GpuMemorySession final : public GsvObject {
public:
    explicit GpuMemorySession(std::shared_ptr<GpuState> state) : GsvObject("gsv:gpu:mem"), m_state(std::move(state)) {
        RegisterHandshake();
        // 101(u64 inicio, u64 fin, ...): memoria accesible por la GPU -> handle
        // 101(u64 primera direccion, u64 ultima, ...) -> handle
        RegisterCommand(101, "CreateGpuAccessibleMemory", [this](IpcContext& ctx) {
            const u32 handle = m_state->next_memory++;
            const u64 first = Pair(ctx, 0), last = Pair(ctx, 2);
            m_state->memories[handle] = {first, last >= first ? last - first + 1 : 0};
            Logger::Log(Logger::Level::Debug, "[gsv] gpu 101: memoria de GPU " + std::to_string(handle) + " en " +
                        Hex(first) + ".." + Hex(last));
            Reply(ctx, {handle});
        });
        RegisterCommand(102, "DestroyGpuAccessibleMemory", [this](IpcContext& ctx) {
            m_state->memories.erase(Word(ctx, 0));
            Done(ctx, 1);
        });
        // 211(?, u64 primera direccion de GPU, u64 ultima, u32 handle, u64 desplazamiento, ..., id en la palabra 15)
        RegisterCommand(211, "NormalMapGpuMemory4K", [this](IpcContext& ctx) {
            const u64 va = Pair(ctx, 1), last = Pair(ctx, 3), offset = Pair(ctx, 6);
            const u32 handle = Word(ctx, 5);
            auto it = m_state->memories.find(handle);
            if (it != m_state->memories.end() && last >= va) {
                auto& gpu = ctx.GetKernel().GetGpu();
                auto& mm = gpu.MemoryManager();
                const u64 cpu = it->second.address + offset, size = last - va + 1;
                gpu.Enqueue([&mm, va, cpu, size] { mm.Map(va, cpu, size); });
                Logger::Log(Logger::Level::Debug, "[gsv] gpu 211: GPU " + Hex(va) + " -> " + Hex(cpu) + " (" + Hex(size) + " bytes)");
            } else {
                Logger::Log(Logger::Level::Warning, "[gsv] gpu 211: memoria " + std::to_string(handle) + " desconocida (GPU " + Hex(va) + ")");
            }
            Done(ctx, 15);
        });
        RegisterCommand(212, "NormalModifyMemoryKind4K", [this](IpcContext& ctx) { Done(ctx, 15); });
        RegisterCommand(213, "NormalUnmapGpuMemory4K", [this](IpcContext& ctx) {
            auto& gpu = ctx.GetKernel().GetGpu();
            auto& mm = gpu.MemoryManager();
            const u64 va = Pair(ctx, 1);
            gpu.Enqueue([&mm, va] { mm.Unmap(va); });
            Done(ctx, 6);
        });
        RegisterCommand(214, "CreateSparseRegion", [this](IpcContext& ctx) {
            Logger::Log(Logger::Level::Info, "[gsv] gpu 214: region solo de GPU " + Hex(Pair(ctx, 0)) + " " + Hex(Pair(ctx, 2)));
            Done(ctx, 9);
        });
    }
private:
    // Termina una operacion: activa el evento del id que va en la palabra 'slot_word'
    void Done(IpcContext& ctx, u32 slot_word) {
        m_state->Event(Word(ctx, slot_word))->signaled = true;
        Reply(ctx, {});
    }
    std::shared_ptr<GpuState> m_state;
};

class GpuSession final : public GsvObject {
public:
    // Direcciones de GPU que puede usar el juego (las mismas que da nvhost-as)
    static constexpr u64 VA_START = 0x8000000ull, VA_END = 0xC000000000ull;

    GpuSession() : GsvObject("gsv:gpu") {
        RegisterHandshake();
        RegisterWords(11, "GetIdRange", {0, GpuState::NUM_IDS - 1});
        RegisterCommand(12, "GetIdEvent", [this](IpcContext& ctx) {
            ctx.PushCopyHandle(ctx.GetKernel().Handles().Create(m_state->Event(Word(ctx, 0))));
            ctx.SetResult(0);
        });
        // Empieza una operacion: aqui no hay nada que hacer, termina ya
        RegisterCommand(21, "Start", [this](IpcContext& ctx) { m_state->Event(Word(ctx, 0))->signaled = true; Reply(ctx, {}); });
        RegisterCommand(22, "OpenMemorySession", [this](IpcContext& ctx) {
            ReplySession(ctx, std::make_shared<GpuMemorySession>(m_state));
        });
        RegisterWords(33, "GetAddressSpaceCount", {1});
        RegisterCommand(41, "CreateGraphicsDevice", [this](IpcContext& ctx) { Reply(ctx, {m_nextDevice++}); });
        RegisterCommand(42, "DestroyGraphicsDevice", [](IpcContext& ctx) { Reply(ctx, {}); });
        // Particiones del espacio de direcciones: la del juego (min, max) y la del sistema (vacia: min > max)
        RegisterCommand(201, "GetUserPartition", [](IpcContext& ctx) { ReplyU64(ctx, {VA_START, VA_END - VA_START}); });
        RegisterCommand(202, "GetGsvPartition", [](IpcContext& ctx) { ReplyU64(ctx, {~0ull, 0}); });
        RegisterCommand(203, "GetVaRange", [](IpcContext& ctx) { ReplyU64(ctx, {1, VA_START, VA_END - VA_START}); });
    }
private:
    std::shared_ptr<GpuState> m_state = std::make_shared<GpuState>();
    u32 m_nextDevice = 1;
};

// ---------------------------------------------------------------------------------------------
// vi2 (gsv 305): pantalla.
// - Eventos "nativos" (31/32, indice 0..7): el juego los ata a una fuente con 261..272. Atado al
//   vsync (261) se activa a 60 Hz; atado a un cambio (263, 265...: refresco, HDR, resolucion) se
//   activa una vez (para que el juego lea el estado) y ya no cambia. Sin atar: a 60 Hz.
// - Buffers de la capa: 212 dice que huecos estan libres; 228 (cancelar) y 229 (presentar)
//   devuelven los que puede usar despues. De momento todos libres y no se muestra nada.
// ---------------------------------------------------------------------------------------------
class Vi2Session final : public GsvObject {
public:
    static constexpr u64 FREE_SLOTS = 0x7FFF;

    Vi2Session() : GsvObject("gsv:vi2") {
        RegisterHandshake();
        RegisterWords(1, "Unknown1", {1});
        RegisterWords(3, "GetDisplayCount", {1});
        // Cuantos "timestamp recorders" (LayerHistory necesita 7)
        RegisterWords(5, "GetTimestampRecorderCount", {7});
        RegisterCommand(11, "GetEvent11", [](IpcContext& ctx) {
            ctx.PushCopyHandle(ctx.GetKernel().Handles().Create(NewEvent("vi2 11")));
            ctx.SetResult(0);
        });
        RegisterCommand(31, "AcquireNativeEvent", [this](IpcContext& ctx) { ReplyNative(ctx, Word(ctx, 0)); });
        RegisterCommand(32, "DuplicateNativeEvent", [this](IpcContext& ctx) { ReplyNative(ctx, Word(ctx, 0)); });
        // (u64 nombre de la pantalla, u32 indice)
        RegisterCommand(261, "BindDisplayVsyncEvent", [this](IpcContext& ctx) { Bind(ctx, Word(ctx, 2), true, false); });
        for (u32 id : {263u, 265u, 267u, 269u, 271u})
            RegisterCommand(id, "BindDisplayChangeEvent", [this](IpcContext& ctx) { Bind(ctx, Word(ctx, 2), false, true); });
        for (u32 id : {262u, 264u, 266u, 268u, 270u, 272u})
            RegisterCommand(id, "UnbindDisplayEvent", [this](IpcContext& ctx) { Bind(ctx, Word(ctx, 2), false, false); });
        // Estado de la pantalla: 60 Hz, sin HDR (formato 1, 400 nits), blanco de referencia 203 nits
        RegisterWords(103, "QueryDisplayRefreshRate", {60, 0});
        RegisterWords(104, "QueryDisplayHdrStatus", {1, 0, 400});
        RegisterWords(105, "QueryDisplayHdrPaperWhite", {203});
        // Resolucion maxima de los buffers: 2 = 1920x1080 (la de la consola en portatil)
        RegisterCommand(106, "GetBufferMaxResolution", [](IpcContext& ctx) { ReplyU64(ctx, {2}); });
        RegisterCommand(212, "GetLayerBufferBits", [](IpcContext& ctx) { ReplyU64(ctx, {FREE_SLOTS}); });
        RegisterCommand(228, "CancelClientBuffer", [](IpcContext& ctx) { Reply(ctx, {u32(FREE_SLOTS), 0, 0, 0, 0, 0}); });
        // 229(u64 token, u32 hueco, u32 imagen, ..., fence: ~syncpoint en la palabra 20, valor en la 22)
        RegisterCommand(229, "PresentClientBuffer", [this](IpcContext& ctx) {
            if (++m_presents <= 4 || m_presents % 600 == 0)
                Logger::Log(Logger::Level::Info, "[gsv] vi2 229: imagen presentada #" + std::to_string(m_presents) +
                            " (hueco " + std::to_string(Word(ctx, 2)) + ", imagen " + std::to_string(Word(ctx, 3)) + ")");
            Reply(ctx, {u32(FREE_SLOTS), 0, 0, 0, 0, 0});
        });
    }

private:
    struct Native { std::shared_ptr<KEvent> event; bool vsync = true; };
    struct VsyncState { std::array<Native, 8> slots{}; bool running = false; };

    void ReplyNative(IpcContext& ctx, u32 index) {
        if (index >= 8) { ctx.SetResult(0xCA01); return; }
        auto& slot = m_vsync->slots[index];
        if (!slot.event) slot.event = NewEvent("vi2 nativo " + std::to_string(index));
        StartVsync(ctx.GetKernel());
        ctx.PushCopyHandle(ctx.GetKernel().Handles().Create(slot.event));
        ctx.SetResult(0);
    }
    void Bind(IpcContext& ctx, u32 index, bool vsync, bool fire_once) {
        if (index < 8 && m_vsync->slots[index].event) {
            m_vsync->slots[index].vsync = vsync;
            if (fire_once) m_vsync->slots[index].event->signaled = true;
        }
        Reply(ctx, {});
    }
    // Un temporizador por sesion: en cada vsync activa los eventos atados al vsync
    void StartVsync(Kernel& k) {
        if (m_tick) return;
        std::weak_ptr<VsyncState> weak = m_vsync;
        auto tick = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak_tick = tick;
        *tick = [&k, weak, weak_tick] {
            auto st = weak.lock();
            auto t = weak_tick.lock();
            if (!st || !t) return;   // la sesion ya no existe
            for (auto& slot : st->slots)
                if (slot.event && slot.vsync) slot.event->signaled = true;
            k.ScheduleAt(Kernel::NextVsync(k.TicksNow()), [weak_tick] { if (auto f = weak_tick.lock()) (*f)(); });
        };
        m_tick = tick;
        k.ScheduleAt(Kernel::NextVsync(k.TicksNow()), [weak_tick] { if (auto f = weak_tick.lock()) (*f)(); });
    }

    std::shared_ptr<VsyncState> m_vsync = std::make_shared<VsyncState>();
    std::shared_ptr<std::function<void()>> m_tick;   // vive lo que la sesion
    u64 m_presents = 0;
};

// ---------------------------------------------------------------------------------------------
// Sesion principal (ISelfController 2000)
// ---------------------------------------------------------------------------------------------
class GraphicsSystemSession final : public GsvObject {
public:
    GraphicsSystemSession() : GsvObject("gsv") {
        RegisterCommand(202, "OpenSyncptSession", [](IpcContext& ctx) { ReplySession(ctx, std::make_shared<SyncptSession>()); });
        RegisterCommand(204, "AttachSyncptMemory", [this](IpcContext& ctx) { AttachSyncptMemory(ctx); });
        RegisterCommand(305, "OpenViSession", [](IpcContext& ctx) { ReplySession(ctx, std::make_shared<Vi2Session>()); });
        RegisterCommand(497, "OpenGpuSession", [](IpcContext& ctx) { ReplySession(ctx, std::make_shared<GpuSession>()); });
        RegisterCommand(1402, "OpenNvsrvSession", [this](IpcContext& ctx) {
            StartResumeTimer(ctx.GetKernel());
            ReplySession(ctx, std::make_shared<NvDrv>("gsv:nvsrv", true));
        });
        // 115(u64 funciones pedidas): las que se conceden (todas)
        RegisterCommand(115, "QueryGrantedFeatures", [](IpcContext& ctx) { Reply(ctx, {Word(ctx, 0), Word(ctx, 1)}); });
        RegisterCommand(1510, "CalculateRequirements", [](IpcContext& ctx) { CalculateRequirements(ctx); });
        RegisterCommand(435, "CalculateNativeImageSize", [](IpcContext& ctx) {
            const ImageLayout l = Layout(ctx);
            ctx.Push<u64>(l.size * std::max<u32>(Word(ctx, 16), 1));
            ctx.SetResult(0);
        });
        // 423(u32 id, u64 direccion, u64 tamano, u32 memoria de GPU, u64 desplazamiento): la memoria
        // nativa 'id' esta en esa direccion
        RegisterCommand(423, "BindNativeGpuMemory", [this](IpcContext& ctx) {
            m_native[Word(ctx, 0)] = {Pair(ctx, 1), Pair(ctx, 3)};
            Logger::Log(Logger::Level::Debug, "[gsv] 423: memoria nativa " + std::to_string(Word(ctx, 0)) + " en " + Hex(Pair(ctx, 1)));
            Reply(ctx, {});
        });
        RegisterCommand(114, "BindNativeMemory", [this](IpcContext& ctx) {
            m_native[Word(ctx, 0)] = {Pair(ctx, 1), Pair(ctx, 3)};
            Reply(ctx, {});
        });
        // 429(preferencia[16], u32 cuantas, u32 memoria nativa) -> id de la primera imagen; la
        // imagen i del grupo es id + i
        RegisterCommand(429, "CreateNativeImage", [this](IpcContext& ctx) {
            const u32 count = std::max<u32>(Word(ctx, 16), 1);
            const u32 id = m_nextImage;
            m_nextImage += count;
            const ImageLayout l = Layout(ctx);
            Logger::Log(Logger::Level::Info, "[gsv] 429: " + std::to_string(count) + " imagen(es) " + std::to_string(l.width) +
                        "x" + std::to_string(l.height) + " en la memoria nativa " + std::to_string(Word(ctx, 17)) + " -> " + std::to_string(id));
            Reply(ctx, {id});
        });
        RegisterCommand(111, "CreateNativeMemory", [this](IpcContext& ctx) {
            Logger::Log(Logger::Level::Info, "[gsv] 111: memoria nativa " + std::to_string(m_nextNative) + " de " + Hex(Pair(ctx, 1)) + " bytes");
            Reply(ctx, {m_nextNative++});
        });
    }

private:
    // --- Pagina de syncpoints (204) ---
    // El juego presta dos memorias: una que escribe el (RW, 0x1000) y otra que solo lee (RO,
    // 0x2000). En la RO va una cabecera (version 1, base, cuantos, y donde estan: los valores
    // u64 de cada syncpoint, los bits "armado" que pone el juego y los bits "atendido" que pone
    // el servidor). Para esperar, el juego hace que su bit difiera del nuestro, activa el primer
    // evento y espera al segundo; nosotros copiamos los valores, igualamos los bits y activamos
    // el segundo. El juego vuelve a mirar el valor y se arma otra vez si aun no ha llegado.
    static constexpr u32 VALUES = 0x40, RO_SUMMARY = 0x1040, RO_BITS = 0x1100, RW_SUMMARY = 0x0, RW_BITS = 0x100;
    struct SyncptPage {
        Kernel* kernel = nullptr;
        Core::Memory* memory = nullptr;
        u64 rw = 0, ro = 0;
        u32 count = 0;
        std::shared_ptr<KEvent> request, notify;
        bool polling = false;
    };

    void AttachSyncptMemory(IpcContext& ctx) {
        Kernel& k = ctx.GetKernel();
        const auto& handles = ctx.Request().copy_handles;
        auto rw = handles.size() > 0 ? k.Handles().Get<KTransferMemory>(handles[0]) : nullptr;
        auto ro = handles.size() > 1 ? k.Handles().Get<KTransferMemory>(handles[1]) : nullptr;
        const u32 count = std::min<u32>(Word(ctx, 1), (0x1000 - VALUES) / 8);
        Logger::Log(Logger::Level::Info, "[gsv] 204: pagina de syncpoints (version " + std::to_string(Word(ctx, 0)) +
                    ", " + std::to_string(count) + " syncpoints)");
        if (!m_page) {
            m_page = std::make_shared<SyncptPage>();
            m_page->request = NewEvent("syncpt request");
            m_page->notify = NewEvent("syncpt notify");
        }
        if (rw && ro && ro->size >= 0x2000) {
            auto& p = *m_page;
            p.kernel = &k;
            p.memory = &ctx.GetMemory();
            p.rw = rw->address;
            p.ro = ro->address;
            p.count = count;
            const u32 header[8] = {1, 0, count, VALUES, RW_SUMMARY, RO_SUMMARY, RW_BITS, RO_BITS};
            p.memory->WriteBytes(p.ro, header, sizeof(header));
            Poll(m_page);
            StartPolling(m_page);
        } else {
            Logger::Log(Logger::Level::Warning, "[gsv] 204: faltan las memorias de transferencia");
        }
        ctx.PushCopyHandle(k.Handles().Create(m_page->request));   // lo activa el juego
        ctx.PushCopyHandle(k.Handles().Create(m_page->notify));    // lo activamos nosotros
        ctx.SetResult(0);
    }

    static void Poll(const std::shared_ptr<SyncptPage>& page) {
        auto& p = *page;
        auto& sp = p.kernel->GetGpu().GetSyncpoints();
        for (u32 id = 0; id < std::min<u32>(p.count, GPU::Syncpoints::COUNT); ++id)
            p.memory->Write<u64>(p.ro + VALUES + id * 8, sp.Read(id));
        bool acked = false;
        for (u32 w = 0; w < (p.count + 63) / 64; ++w) {
            const u64 bits = p.memory->Read<u64>(p.rw + RW_BITS + w * 8);
            if (p.memory->Read<u64>(p.ro + RO_BITS + w * 8) != bits) {
                p.memory->Write<u64>(p.ro + RO_BITS + w * 8, bits);
                acked = true;
            }
        }
        p.memory->Write<u64>(p.ro + RO_SUMMARY, p.memory->Read<u64>(p.rw + RW_SUMMARY));
        if (acked) p.notify->signaled = true;
        p.request->signaled = false;
    }
    // Cada 0,5 ms mientras exista la sesion
    static void StartPolling(const std::shared_ptr<SyncptPage>& page) {
        if (page->kernel == nullptr || page->notify == nullptr) return;
        static constexpr u64 PERIOD = Core::Interpreter::TICK_FREQUENCY / 2000;
        std::weak_ptr<SyncptPage> weak = page;
        Kernel* k = page->kernel;
        auto tick = std::make_shared<std::function<void()>>();
        *tick = [weak, k, tick] {
            auto p = weak.lock();
            if (!p) return;
            Poll(p);
            k->ScheduleAt(k->TicksNow() + PERIOD, *tick);
        };
        if (!page->polling) { page->polling = true; k->ScheduleAt(k->TicksNow() + PERIOD, *tick); }
    }

    // --- Canales de la GPU esperando un semaforo que suelta la CPU ---
    // La CPU solo escribe en memoria (no avisa): cada 0,5 ms se mira si alguno puede seguir.
    void StartResumeTimer(Kernel& k) {
        if (m_resumeTick) return;
        static constexpr u64 PERIOD = Core::Interpreter::TICK_FREQUENCY / 2000;
        auto tick = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak = tick;
        auto queued = std::make_shared<std::atomic<bool>>(false);
        *tick = [&k, weak, queued] {
            auto& gpu = k.GetGpu();
            if (gpu.HasWaitingChannels() && !queued->exchange(true))
                gpu.Enqueue([&gpu, queued] { queued->store(false); gpu.ResumeWaitingChannels(); });
            k.ScheduleAt(k.TicksNow() + PERIOD, [weak] { if (auto f = weak.lock()) (*f)(); });
        };
        m_resumeTick = tick;
        k.ScheduleAt(k.TicksNow() + PERIOD, [weak] { if (auto f = weak.lock()) (*f)(); });
    }
    std::shared_ptr<std::function<void()>> m_resumeTick;

    // --- Tamano de imagenes (1510, 435) ---
    // Preferencia de imagen (16 palabras): tipo en la 1 (2 = block linear), (ancho-1) | (alto-1) << 16
    // en la 2, formato en la 6. 4 bytes por pixel. Block linear: "GOBs" de 64 bytes x 8 filas,
    // apilados en bloques de hasta 16 GOBs de alto.
    struct ImageLayout { u32 width, height; u64 size, alignment; };
    static ImageLayout Layout(IpcContext& ctx) {
        const u32 packed = Word(ctx, 2);
        const u32 width = (packed & 0xFFFF) + 1, height = (packed >> 16) + 1;
        const u64 row_bytes = u64(width) * 4;
        if (Word(ctx, 1) != 2) {   // lineal: filas de 256 bytes
            const u64 pitch = (row_bytes + 255) & ~u64(255);
            return {width, height, (pitch * height + 0xFFF) & ~u64(0xFFF), 0x1000};
        }
        u32 block_gobs = 1;
        while (block_gobs < 16 && block_gobs * 8 < height) block_gobs *= 2;
        const u64 block_rows = 8ull * block_gobs;
        const u64 pitch = (row_bytes + 63) & ~u64(63);
        const u64 rows = (height + block_rows - 1) / block_rows * block_rows;
        const u64 align = 512ull * block_gobs;   // un bloque de un GOB de ancho
        const u64 size = (pitch * rows + 0xFFF) & ~u64(0xFFF);
        return {width, height, size, std::max<u64>(align, 0x1000)};
    }
    static void CalculateRequirements(IpcContext& ctx) {
        const ImageLayout l = Layout(ctx);
        Logger::Log(Logger::Level::Debug, "[gsv] 1510: imagen " + std::to_string(l.width) + "x" + std::to_string(l.height) +
                    " formato " + std::to_string(Word(ctx, 6)) + " -> " + Hex(l.size) + " bytes");
        ReplyU64(ctx, {l.size, l.alignment});
    }

    std::shared_ptr<SyncptPage> m_page;
    u32 m_nextNative = 1;
    u32 m_nextImage = 1;
    struct NativeMemory { u64 address, size; };
    std::map<u32, NativeMemory> m_native;
};

} // namespace

std::shared_ptr<ServiceObject> CreateGraphicsSystemSession() {
    return std::make_shared<GraphicsSystemSession>();
}

} // namespace NeXo2::HLE
