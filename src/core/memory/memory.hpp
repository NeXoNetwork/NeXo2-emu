#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
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
    Shared       = 0x06, // memoria compartida con un servicio (svcMapSharedMemory)
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
        for (auto& l2 : m_table) l2.reset();
        m_regions.clear();
        m_codeWrites = 0;
        m_pendingCount = 0;
        ++m_generation;
    }

    // Cambia cada vez que se borran las paginas: quien guarde punteros a paginas
    // (la cache de instrucciones de la CPU) sabe asi que ya no valen.
    u64 Generation() const { return m_generation; }

    // ---------------------------------------------------------------------
    // Paginas de codigo. La CPU guarda instrucciones ya decodificadas por pagina;
    // si el programa escribe en una de esas paginas, hay que tirar esa cache.
    //   MarkCode(addr)    -> la CPU avisa: "tengo esta pagina decodificada"
    //   CodeWriteCount()  -> sube cada vez que se escribe en una pagina marcada
    //   TakeCodeWrites()  -> que paginas eran (y se olvida de ellas)
    // ---------------------------------------------------------------------
    void MarkCode(VAddr addr) {
        if (addr >= ADDRESS_SPACE) return;
        GetOrCreatePage(addr / PAGE_SIZE)->code = true;
    }
    u64 CodeWriteCount() const { return m_codeWrites; }
    // Variable que se pone a true en cada escritura en codigo (la CPU la mira en su bucle)
    void SetCodeWriteFlag(bool* flag) { m_codeWriteFlag = flag; }

    // Copia en 'out' las paginas de codigo escritas (direccion base de cada una).
    // Devuelve false si fueron demasiadas para recordarlas: entonces hay que tirarlo todo.
    template <size_t N>
    bool TakeCodeWrites(std::array<VAddr, N>& out, size_t& count) {
        const bool overflow = m_pendingCount > m_pending.size();
        count = std::min(m_pendingCount, std::min(N, m_pending.size()));
        for (size_t i = 0; i < count; ++i) out[i] = m_pending[i];
        m_pendingCount = 0;
        return !overflow;
    }

    // Puntero a los 4 KB de la pagina que contiene 'addr', o nullptr si no existe.
    // Valido hasta el siguiente Clear().
    u8* PagePointer(VAddr addr) {
        if (addr >= ADDRESS_SPACE) return nullptr;
        Page* page = FindPage(addr / PAGE_SIZE);
        return page ? page->bytes.data() : nullptr;
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

    // Lectura/escritura de un valor. Camino rapido: el valor cabe entero en una pagina
    // (casi siempre). Si cruza el borde entre dos paginas, va por ReadBytes/WriteBytes.
    template <typename T>
    T Read(VAddr addr) {
        T value{};
        const u64 offset = addr & (PAGE_SIZE - 1);
        if (offset + sizeof(T) <= PAGE_SIZE && addr < ADDRESS_SPACE) {
            if (const Page* page = FindPage(addr / PAGE_SIZE))
                std::memcpy(&value, page->bytes.data() + offset, sizeof(T));
            return value;
        }
        ReadBytes(addr, &value, sizeof(T));
        return value;
    }

    template <typename T>
    void Write(VAddr addr, T value) {
        const u64 offset = addr & (PAGE_SIZE - 1);
        if (offset + sizeof(T) <= PAGE_SIZE && addr < ADDRESS_SPACE) {
            Page* page = GetOrCreatePage(addr / PAGE_SIZE);
            if (page->code) [[unlikely]] NoteCodeWrite(page, addr);
            std::memcpy(page->bytes.data() + offset, &value, sizeof(T));
            return;
        }
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
                std::memcpy(out, page->bytes.data() + offset, chunk);
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
            if (page->code) [[unlikely]] NoteCodeWrite(page, addr);
            std::memcpy(page->bytes.data() + offset, in, chunk);
            addr += chunk; in += chunk; size -= chunk;
        }
    }

private:
    struct Page {
        std::array<u8, PAGE_SIZE> bytes{};   // los 4 KB (a cero al crearla)
        bool code = false;                   // la CPU tiene instrucciones decodificadas de aqui
    };

    // Primera escritura en una pagina de codigo: se apunta y se desmarca (las
    // siguientes escrituras ya no cuentan hasta que la CPU la vuelva a decodificar).
    void NoteCodeWrite(Page* page, VAddr addr) {
        page->code = false;
        if (m_pendingCount < m_pending.size()) m_pending[m_pendingCount] = addr & ~(PAGE_SIZE - 1);
        ++m_pendingCount;
        ++m_codeWrites;
        if (m_codeWriteFlag) *m_codeWriteFlag = true;
    }

    // Tabla de paginas de dos niveles (como la de una CPU real):
    //   nivel 1: un hueco por cada 2 MB  (12 GB / 2 MB = 6144 huecos)
    //   nivel 2: 512 punteros a paginas de 4 KB, creado solo si hace falta
    // Buscar una pagina son dos accesos a array, mucho mas rapido que un unordered_map.
    static constexpr u64 L2_BITS = 9;                       // 512 paginas = 2 MB
    static constexpr u64 L2_SIZE = 1ull << L2_BITS;
    static constexpr u64 L1_SIZE = (ADDRESS_SPACE / PAGE_SIZE) >> L2_BITS;
    using L2Table = std::array<Page*, L2_SIZE>;

    Page* FindPage(u64 index) const {
        const L2Table* l2 = m_table[index >> L2_BITS].get();
        return l2 ? (*l2)[index & (L2_SIZE - 1)] : nullptr;
    }

    Page* GetOrCreatePage(u64 index) {
        auto& l2 = m_table[index >> L2_BITS];
        if (!l2) l2 = std::make_unique<L2Table>(); // todos los punteros a nullptr
        Page*& slot = (*l2)[index & (L2_SIZE - 1)];
        if (!slot) {
            auto page = std::make_unique<Page>(); // página nueva inicializada a 0
            slot = page.get();
            m_pages.push_back(std::move(page));
        }
        return slot;
    }

    std::vector<std::unique_ptr<Page>> m_pages;                      // duenas de las paginas
    std::array<std::unique_ptr<L2Table>, L1_SIZE> m_table{};
    u64 m_generation = 0;
    u64 m_codeWrites = 0;                       // escrituras en paginas de codigo
    std::array<VAddr, 8> m_pending{};           // que paginas eran
    size_t m_pendingCount = 0;
    bool* m_codeWriteFlag = nullptr;
    std::map<VAddr, MemoryRegion> m_regions; // ordenadas por direccion
};

} // namespace NeXo2::Core
