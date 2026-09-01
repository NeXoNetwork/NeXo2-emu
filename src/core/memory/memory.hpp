#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <unordered_map>
#include "common/types.hpp"

namespace NeXo2::Core {

// Gestor de memoria por paginación (VMM básico).
// En vez de reservar 12 GB de golpe (lo que petaría en muchos PCs), las
// páginas de 4 KB se crean bajo demanda la primera vez que se escriben.
class Memory {
public:
    static constexpr u64 ADDRESS_SPACE = 12ULL * 1024 * 1024 * 1024; // 12 GB
    static constexpr u64 PAGE_SIZE     = 0x1000;                     // 4 KB

    Memory() = default;

    bool   IsReady() const { return true; }
    size_t AllocatedPages() const { return m_pages.size(); }
    u64    AllocatedBytes() const { return static_cast<u64>(m_pages.size()) * PAGE_SIZE; }

    template <typename T>
    T Read(VAddr addr) {
        T value{};
        ReadBytes(addr, &value, sizeof(T));
        return value;
    }

    template <typename T>
    void Write(VAddr addr, T value) {
        WriteBytes(addr, &value, sizeof(T));
    }

    // Lee 'size' bytes. Las zonas no mapeadas se devuelven como ceros.
    void ReadBytes(VAddr addr, void* dst, size_t size) {
        auto out = static_cast<u8*>(dst);
        while (size > 0) {
            if (addr >= ADDRESS_SPACE) { std::memset(out, 0, size); return; }
            const u64 index  = addr / PAGE_SIZE;
            const u64 offset = addr % PAGE_SIZE;
            const size_t chunk = std::min<size_t>(size, PAGE_SIZE - offset);
            if (Page* page = FindPage(index))
                std::memcpy(out, page->data() + offset, chunk);
            else
                std::memset(out, 0, chunk);
            addr += chunk; out += chunk; size -= chunk;
        }
    }

    // Escribe 'size' bytes, creando las páginas necesarias.
    void WriteBytes(VAddr addr, const void* src, size_t size) {
        auto in = static_cast<const u8*>(src);
        while (size > 0) {
            if (addr >= ADDRESS_SPACE) return; // fuera del espacio de direcciones
            const u64 index  = addr / PAGE_SIZE;
            const u64 offset = addr % PAGE_SIZE;
            const size_t chunk = std::min<size_t>(size, PAGE_SIZE - offset);
            Page* page = GetOrCreatePage(index);
            std::memcpy(page->data() + offset, in, chunk);
            addr += chunk; in += chunk; size -= chunk;
        }
    }

private:
    using Page = std::array<u8, PAGE_SIZE>;

    Page* FindPage(u64 index) {
        auto it = m_pages.find(index);
        return it == m_pages.end() ? nullptr : it->second.get();
    }

    Page* GetOrCreatePage(u64 index) {
        auto& slot = m_pages[index];
        if (!slot) slot = std::make_unique<Page>(); // página nueva inicializada a 0
        return slot.get();
    }

    std::unordered_map<u64, std::unique_ptr<Page>> m_pages;
};

} // namespace NeXo2::Core
