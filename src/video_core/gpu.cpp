#include <atomic>
#include "gpu.hpp"
#include "engines.hpp"
#include "memory.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace NeXo2::GPU {

using Common::Logger;

// ============================================================================
//  Memoria virtual de la GPU
// ============================================================================

u64 GpuMemoryManager::Allocate(u64 size, u64 align, bool big_pages) {
    if (size == 0) return 0;
    const u64 page = big_pages ? BIG_PAGE : SMALL_PAGE;
    if (align < page) align = page;
    size = (size + page - 1) & ~(page - 1);
    const u64 lo = big_pages ? BIG_REGION_START : SMALL_REGION_START;
    const u64 hi = big_pages ? BIG_REGION_END : SMALL_REGION_END;
    // Primer hueco libre (los huecos ocupados estan ordenados)
    u64 candidate = (lo + align - 1) & ~(align - 1);
    for (const auto& [start, len] : m_reserved) {
        if (start + len <= candidate) continue;
        if (start >= candidate + size) break;
        candidate = (start + len + align - 1) & ~(align - 1);
    }
    if (candidate + size > hi) return 0;
    m_reserved[candidate] = size;
    return candidate;
}

bool GpuMemoryManager::ReserveFixed(u64 va, u64 size) {
    // Si ya esta reservado (o se solapa) se acepta igual: algunos programas reservan
    // un espacio grande y luego proyectan buffers dentro con direccion fija.
    auto it = m_reserved.upper_bound(va);
    if (it != m_reserved.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second > va) return true;
    }
    if (it != m_reserved.end() && it->first < va + size) return true;
    m_reserved[va] = size;
    return true;
}

void GpuMemoryManager::Free(u64 va, u64 size) {
    (void)size;
    m_reserved.erase(va);
}

u64 GpuMemoryManager::NextGeneration() {
    static std::atomic<u64> counter{0};
    return ++counter;
}

void GpuMemoryManager::Map(u64 va, u64 cpu_addr, u64 size) {
    m_generation = NextGeneration();
    // Quitar lo que se solape (una proyeccion nueva sustituye a la vieja)
    auto it = m_maps.lower_bound(va);
    if (it != m_maps.begin() && std::prev(it)->first + std::prev(it)->second.size > va) --it;
    while (it != m_maps.end() && it->first < va + size) it = m_maps.erase(it);
    m_maps[va] = Mapping{size, cpu_addr};
}

u64 GpuMemoryManager::Unmap(u64 va) {
    m_generation = NextGeneration();
    auto it = m_maps.find(va);
    if (it == m_maps.end()) return 0;
    const u64 size = it->second.size;
    m_maps.erase(it);
    return size;
}

const std::pair<const u64, GpuMemoryManager::Mapping>* GpuMemoryManager::Find(u64 va) const {
    // Ultima proyeccion encontrada (una por hilo): casi todos los accesos seguidos caen en la misma
    thread_local const GpuMemoryManager* t_owner = nullptr;
    thread_local u64 t_generation = 0;
    thread_local const std::pair<const u64, Mapping>* t_last = nullptr;
    if (t_owner == this && t_generation == m_generation && va >= t_last->first && va - t_last->first < t_last->second.size)
        return t_last;
    auto it = m_maps.upper_bound(va);
    if (it == m_maps.begin()) return nullptr;
    --it;
    if (va >= it->first + it->second.size) return nullptr;
    t_owner = this;
    t_generation = m_generation;
    t_last = &*it;
    return t_last;
}

std::optional<u64> GpuMemoryManager::Translate(u64 va) const {
    const auto* m = Find(va);
    if (!m) return std::nullopt;
    return m->second.cpu + (va - m->first);
}

void GpuMemoryManager::ReadBlock(u64 va, void* dst, size_t size) const {
    auto* out = static_cast<u8*>(dst);
    while (size > 0) {
        const auto* m = Find(va);
        if (!m) { std::memset(out, 0, size); return; }   // sin proyectar: ceros
        const u64 off = va - m->first;
        const size_t chunk = size_t(std::min<u64>(size, m->second.size - off));
        m_memory.ReadBytes(m->second.cpu + off, out, chunk);
        va += chunk; out += chunk; size -= chunk;
    }
}

void GpuMemoryManager::Touch(u64 va, u64 size) {
    while (size > 0) {
        const auto* m = Find(va);
        if (!m) return;
        const u64 off = va - m->first;
        const u64 chunk = std::min<u64>(size, m->second.size - off);
        m_memory.TouchPages(m->second.cpu + off, chunk);
        va += chunk; size -= chunk;
    }
}

void GpuMemoryManager::WriteBlock(u64 va, const void* src, size_t size) {
    auto* in = static_cast<const u8*>(src);
    while (size > 0) {
        const auto* m = Find(va);
        if (!m) return;                                    // sin proyectar: se pierde
        const u64 off = va - m->first;
        const size_t chunk = size_t(std::min<u64>(size, m->second.size - off));
        m_memory.WriteBytes(m->second.cpu + off, in, chunk);
        va += chunk; in += chunk; size -= chunk;
    }
}

// ============================================================================
//  Syncpoints
// ============================================================================

void Syncpoints::Increment(u32 id) {
    if (id >= COUNT) return;
    ++m_values[id];
    for (size_t i = 0; i < m_waiters.size();) {
        if (m_waiters[i].id == id && Reached(id, m_waiters[i].threshold)) {
            auto fire = std::move(m_waiters[i].fire);
            m_waiters.erase(m_waiters.begin() + std::ptrdiff_t(i));
            fire();
        } else {
            ++i;
        }
    }
}

void Syncpoints::AddWaiter(u32 id, u32 threshold, std::function<void()> fire) {
    if (Reached(id, threshold)) { fire(); return; }
    m_waiters.push_back({id, threshold, std::move(fire)});
}

// ============================================================================
//  Canal: GPFIFO + pushbuffers
// ============================================================================

Channel::Channel(Gpu& gpu, u32 id)
    : m_gpu(gpu), m_id(id), m_syncpoint(gpu.GetSyncpoints().AllocateSyncpoint()),
      m_3d(std::make_unique<Maxwell3D>(gpu, m_syncpoint)),
      m_dma(std::make_unique<MaxwellDma>(gpu)),
      m_2d(std::make_unique<Fermi2D>(gpu)),
      m_compute(std::make_unique<KeplerCompute>(gpu)),
      m_inline(std::make_unique<InlineToMemory>(gpu)) {}

Channel::~Channel() = default;

void Channel::BindSubchannel(u32 subchannel, u32 class_id) {
    Engine* e = nullptr;
    switch (class_id & 0xFFFF) {
        case 0xB197: e = m_3d.get(); break;
        case 0xB0B5: e = m_dma.get(); break;
        case 0x902D: e = m_2d.get(); break;
        case 0xB1C0: e = m_compute.get(); break;
        case 0xA140: e = m_inline.get(); break;
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "clase 0x%04X desconocida", class_id & 0xFFFF);
            m_gpu.Warn("class" + std::to_string(class_id), buf);
            break;
        }
    }
    m_subchannels[subchannel & 7] = e;
}

void Channel::SubmitGpfifo(const std::vector<u64>& entries) {
    ++m_gpu.GetStats().submits;
    for (u64 entry : entries) {
        const u64 va = entry & 0xFF'FFFF'FFFCull;
        const u32 words = u32((entry >> 42) & 0x1FFFFF);
        if (words) ProcessPushbuffer(va, words);
    }
}

void Channel::ProcessPushbuffer(u64 va, u32 words) {
    std::vector<u32> cmds(words);
    m_gpu.MemoryManager().ReadBlock(va, cmds.data(), size_t(words) * 4);
    size_t i = 0;
    while (i < cmds.size()) {
        const u32 header = cmds[i++];
        const u32 method = header & 0x1FFF;
        const u32 subch = (header >> 13) & 7;
        const u32 count = (header >> 16) & 0x1FFF;
        const u32 mode = header >> 29;
        switch (mode) {
            case 1:   // incrementar: cada palabra al metodo siguiente
                for (u32 k = 0; k < count && i < cmds.size(); ++k)
                    CallMethod(subch, method + k, cmds[i++], k + 1 == count);
                break;
            case 3:   // no incrementar: todas al mismo metodo
                for (u32 k = 0; k < count && i < cmds.size(); ++k)
                    CallMethod(subch, method, cmds[i++], k + 1 == count);
                break;
            case 4:   // inmediato: el dato va en la cabecera
                CallMethod(subch, method, count, true);
                break;
            case 5:   // incrementar una vez: la primera al metodo, las demas al siguiente
                for (u32 k = 0; k < count && i < cmds.size(); ++k)
                    CallMethod(subch, method + (k ? 1 : 0), cmds[i++], k + 1 == count);
                break;
            case 0:   // "usar tercer opcode" (nop / continuacion): se salta la palabra
                break;
            default: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "pushbuffer: modo %u desconocido", mode);
                m_gpu.Warn("pbmode" + std::to_string(mode), buf);
                i += count;
                break;
            }
        }
    }
}

void Channel::CallMethod(u32 subchannel, u32 method, u32 arg, bool last) {
    ++m_gpu.GetStats().methods;
    if (method < 0x40) {   // metodos del propio canal (iguales en todas las subcanales)
        if (method == 0x00) BindSubchannel(subchannel, arg);
        else HostMethod(method, arg);
        return;
    }
    Engine* e = m_subchannels[subchannel & 7];
    if (!e) {
        ++m_gpu.GetStats().unknown_methods;
        m_gpu.Warn("nosubch" + std::to_string(subchannel), "metodo para una subcanal sin motor asignado");
        return;
    }
    e->CallMethod(method, arg, last);
}

void Channel::HostMethod(u32 method, u32 arg) {
    m_host[method] = arg;
    switch (method) {
        case 0x07: {   // SEMAPHORE_D: ejecutar operacion de semaforo
            const u64 addr = (u64(m_host[0x04] & 0xFF) << 32) | m_host[0x05];
            const u32 payload = m_host[0x06];
            const u32 op = arg & 0x1F;
            auto& mm = m_gpu.MemoryManager();
            if (op == 2) {             // release
                mm.Write<u32>(addr, payload);
                if (!((arg >> 24) & 1)) {   // 16 bytes: valor + marca de tiempo
                    mm.Write<u32>(addr + 4, 0);
                    mm.Write<u64>(addr + 8, 0);
                }
            } else if (op == 1 || op == 4 || op == 8) {   // acquire: esperar a que se cumpla
                const u32 v = mm.Read<u32>(addr);
                const bool ok = op == 1 ? v == payload : op == 4 ? s32(v - payload) >= 0 : (v & payload) != 0;
                if (!ok) m_gpu.Warn("semacq", "semaforo no cumplido (se sigue: la GPU es sincrona)");
            } else if (op == 16) {     // reduccion (suma...): poco usado
                m_gpu.Warn("semred", "semaforo con reduccion no implementado");
            }
            break;
        }
        case 0x1D: {   // SYNCPOINT_B
            const u32 id = (arg >> 8) & 0xFF;
            if (arg & 1) m_gpu.GetSyncpoints().Increment(id);
            else if (!m_gpu.GetSyncpoints().Reached(id, m_host[0x1C]))
                m_gpu.Warn("syncwait", "espera de syncpoint no cumplida (se sigue: la GPU es sincrona)");
            break;
        }
        default:
            break;   // referencias, interrupciones, vaciados de cache...: nada que hacer
    }
}

// ============================================================================
//  Gpu
// ============================================================================

Gpu::Gpu(Core::Memory& memory) : m_memory(memory), m_mm(memory) {}
Gpu::~Gpu() = default;

Channel& Gpu::CreateChannel() {
    m_channels.push_back(std::make_unique<Channel>(*this, u32(m_channels.size())));
    return *m_channels.back();
}

Channel* Gpu::GetChannel(u32 id) {
    return id < m_channels.size() ? m_channels[id].get() : nullptr;
}

void Gpu::Reset() {
    m_channels.clear();
    m_mm.Reset();
    m_syncpoints.Reset();
    m_stats = {};
    m_warned.clear();
}

void Gpu::Warn(const std::string& key, const std::string& message) {
    if (m_warned[key]) return;
    m_warned[key] = true;
    Logger::Log(Logger::Level::Warning, "[GPU] " + message);
}

} // namespace NeXo2::GPU
