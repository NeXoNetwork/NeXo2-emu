#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include "common/types.hpp"

namespace NeXo2::Core {

// Permisos de una zona de memoria (mismos valores que Horizon: R=1, W=2, X=4).
enum class MemoryPermission : u32 {
    None        = 0,
    Read        = 1,
    ReadWrite   = 3,
    ReadExecute = 5,
};

// Tipo de zona, con los valores de Horizon (svc::MemoryState, ver docs/02-horizon-os/svc.md).
// svcQueryMemory devuelve este numero a los programas.
enum class MemoryState : u32 {
    Free         = 0x00, // sin mapear
    Static       = 0x02, // memoria fija del sistema (aqui: datos del loader)
    Code         = 0x03, // .text y .rodata del programa
    CodeData     = 0x04, // .data y .bss del programa
    Normal       = 0x05, // heap (svcSetHeapSize)
    Stack        = 0x0B, // pila
    ThreadLocal  = 0x0C, // TLS de los hilos
    Inaccessible = 0x10, // fuera del espacio de direcciones
};

// Una zona contigua de memoria con el mismo tipo y permisos.
struct MemoryRegion {
    VAddr            base  = 0;
    u64              size  = 0;
    MemoryState      state = MemoryState::Free;
    MemoryPermission perm  = MemoryPermission::None;
    std::string      name;          // solo para depurar ("pila", ".text"...)
    VAddr End() const { return base + size; }
};

// Gestor de memoria por paginación (VMM básico).
// En vez de reservar 12 GB de golpe (lo que petaría en muchos PCs), las
// páginas de 4 KB se crean bajo demanda la primera vez que se escriben.
class Memory {
public:
    static constexpr u64 ADDRESS_SPACE = 12ULL * 1024 * 1024 * 1024; // 12 GB
    static constexpr u64 PAGE_SIZE     = 0x1000;                     // 4 KB

    Memory() = default;

    // Borra todo (paginas y zonas). Se usa al cargar un programa nuevo.
    void Clear() {
        m_pages.clear();
        m_regions.clear();
    }

    // ---------------------------------------------------------------------
    // Mapa de zonas. De momento es solo informativo (para svcQueryMemory y
    // la interfaz): los permisos todavia no se comprueban al leer/escribir.
    // ---------------------------------------------------------------------

    // Marca [base, base+size) como una zona. Si pisa zonas existentes, las recorta.
    void MapRegion(VAddr base, u64 size, MemoryState state, MemoryPermission perm, std::string name) {
        UnmapRegion(base, size);
        if (size == 0) return;
        m_regions[base] = MemoryRegion{base, size, state, perm, std::move(name)};
    }

    // Quita [base, base+size) del mapa (partiendo las zonas que lo crucen).
    void UnmapRegion(VAddr base, u64 size) {
        const VAddr end = base + size;
        auto it = m_regions.lower_bound(base);
        if (it != m_regions.begin()) --it; // la zona anterior puede empezar antes y cruzar 'base'
        while (it != m_regions.end() && it->second.base < end) {
            const MemoryRegion r = it->second;
            if (r.End() <= base) { ++it; continue; }
            it = m_regions.erase(it);
            if (r.base < base) {           // trozo que queda a la izquierda
                MemoryRegion left = r;
                left.size = base - r.base;
                m_regions[left.base] = left;
            }
            if (r.End() > end) {           // trozo que queda a la derecha
                MemoryRegion right = r;
                right.base = end;
                right.size = r.End() - end;
                m_regions[right.base] = right;
                break;
            }
        }
    }

    // Devuelve la zona que contiene 'addr'. Si no hay ninguna, devuelve el hueco
    // libre (Free) que la rodea, como hace Horizon.
    MemoryRegion QueryRegion(VAddr addr) const {
        if (addr >= ADDRESS_SPACE)
            return {ADDRESS_SPACE, 0 - ADDRESS_SPACE, MemoryState::Inaccessible, MemoryPermission::None, "inaccesible"};
        auto next = m_regions.upper_bound(addr);
        VAddr free_start = 0;
        if (next != m_regions.begin()) {
            const MemoryRegion& prev = std::prev(next)->second;
            if (addr < prev.End()) return prev;
            free_start = prev.End();
        }
        const VAddr free_end = (next != m_regions.end()) ? next->second.base : ADDRESS_SPACE;
        return {free_start, free_end - free_start, MemoryState::Free, MemoryPermission::None, "libre"};
    }

    const std::map<VAddr, MemoryRegion>& Regions() const { return m_regions; }

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
    std::map<VAddr, MemoryRegion> m_regions; // ordenadas por direccion
};

} // namespace NeXo2::Core
