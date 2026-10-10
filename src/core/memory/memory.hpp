#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
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
//
// Varios hilos: la CPU emulada y la GPU (en su propio hilo) leen y escriben a la vez.
// Buscar una pagina no usa candado (punteros atomicos); crear una si (m_pageMutex).
// Clear() y el mapa de zonas solo se usan con la GPU parada.
class Memory {
public:
    static constexpr u64 ADDRESS_SPACE = 12ULL * 1024 * 1024 * 1024; // 12 GB
    static constexpr u64 PAGE_SIZE     = 0x1000;                     // 4 KB

    Memory() = default;

    // Borra todo (paginas y zonas). Se usa al cargar un programa nuevo.
    void Clear() {
        std::scoped_lock lock(m_pageMutex, m_codeMutex);
        for (auto& l2 : m_table) l2.store(nullptr, std::memory_order_relaxed);
        m_l2Tables.clear();
        m_pages.clear();
        if (m_flat) std::fill_n(m_flat.get(), FLAT_ENTRIES, nullptr);
        m_regions.clear();
        m_codeWrites = 0;
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
    //   TakeCodeWrites()  -> que paginas eran desde la ultima vez que pregunto ESA CPU
    // Con varios nucleos cada CPU lleva su propio "cursor": las escrituras se guardan
    // en un anillo y cada una lee las que le faltan.
    // ---------------------------------------------------------------------
    void MarkCode(VAddr addr) {
        if (addr >= ADDRESS_SPACE) return;
        const u64 index = addr / PAGE_SIZE;
        GetOrCreatePage(index)->code.store(true, std::memory_order_relaxed);
        SetFlat(index, nullptr);   // el JIT escribira por Write(): asi nos enteramos
    }

    // ---------------------------------------------------------------------
    // Tabla de paginas "plana" para el JIT: un puntero por pagina de 4 KB
    // (2^22 entradas = 16 GB de direcciones). El codigo generado por el JIT la
    // consulta directamente: si la entrada no es nullptr, lee/escribe la memoria
    // sin llamar a ninguna funcion. Entradas a nullptr (pagina que no existe, o
    // pagina con codigo ya traducido) -> el JIT llama a Read()/Write().
    // ---------------------------------------------------------------------
    static constexpr unsigned FLAT_BITS = 34;                       // bits de direccion cubiertos
    static constexpr u64 FLAT_ENTRIES = 1ull << (FLAT_BITS - 12);
    void** EnableFlatPageTable() {
        std::lock_guard lock(m_pageMutex);
        if (!m_flat) {
            m_flat = std::make_unique<void*[]>(FLAT_ENTRIES);      // todo a nullptr
            for (u64 l1 = 0; l1 < L1_SIZE; ++l1) {
                L2Table* l2 = m_table[l1].load(std::memory_order_acquire);
                if (!l2) continue;
                for (u64 i = 0; i < L2_SIZE; ++i) {
                    Page* p = (*l2)[i].load(std::memory_order_acquire);
                    if (p && !p->code.load(std::memory_order_relaxed)) m_flat[(l1 << L2_BITS) | i] = p->bytes.data();
                }
            }
        }
        return m_flat.get();
    }
    u64 CodeWriteCount() const { return m_codeWrites.load(std::memory_order_acquire); }
    // Variables que se ponen a true en cada escritura en codigo (cada CPU mira la suya en su bucle)
    void AddCodeWriteFlag(std::atomic<bool>* flag) {
        std::lock_guard lock(m_codeMutex);
        m_codeWriteFlags.push_back(flag);
    }
    void RemoveCodeWriteFlag(std::atomic<bool>* flag) {
        std::lock_guard lock(m_codeMutex);
        std::erase(m_codeWriteFlags, flag);
    }

    // Copia en 'out' las paginas de codigo escritas desde 'cursor' (lo que ya vio esa CPU)
    // y adelanta el cursor. Devuelve false si fueron demasiadas: entonces hay que tirarlo todo.
    template <size_t N>
    bool TakeCodeWrites(u64& cursor, std::array<VAddr, N>& out, size_t& count) {
        std::lock_guard lock(m_codeMutex);
        const u64 now = m_codeWrites.load(std::memory_order_relaxed);
        const u64 pending = now - cursor;
        cursor = now;
        count = 0;
        if (pending > N || pending > m_ring.size()) return false;
        for (u64 i = now - pending; i < now; ++i) out[count++] = m_ring[i % m_ring.size()];
        return true;
    }

    // Mueve las paginas de [src, src+size) a [dst, dst+size): los datos pasan a verse en 'dst'
    // y 'src' queda sin paginas. Es lo que hace svcMapMemory (la memoria original queda
    // inaccesible mientras esta mapeada en otro sitio) y, al reves, svcUnmapMemory.
    void MovePages(VAddr dst, VAddr src, u64 size) {
        if (dst + size > ADDRESS_SPACE || src + size > ADDRESS_SPACE) return;
        std::lock_guard lock(m_pageMutex);
        for (u64 off = 0; off < size; off += PAGE_SIZE) {
            const u64 si = (src + off) / PAGE_SIZE, di = (dst + off) / PAGE_SIZE;
            L2Table* sl2 = m_table[si >> L2_BITS].load(std::memory_order_relaxed);
            Page* page = sl2 ? (*sl2)[si & (L2_SIZE - 1)].exchange(nullptr, std::memory_order_acq_rel) : nullptr;
            SetFlat(si, nullptr);
            auto& dl1 = m_table[di >> L2_BITS];
            L2Table* dl2 = dl1.load(std::memory_order_relaxed);
            if (!dl2) {
                if (!page) continue;
                m_l2Tables.push_back(std::make_unique<L2Table>());
                dl2 = m_l2Tables.back().get();
                dl1.store(dl2, std::memory_order_release);
            }
            (*dl2)[di & (L2_SIZE - 1)].store(page, std::memory_order_release);
            SetFlat(di, page && !page->code.load(std::memory_order_relaxed) ? page->bytes.data() : nullptr);
        }
        ++m_generation;   // las CPUs guardan instrucciones por direccion: que lo tiren todo
    }

    // ---------------------------------------------------------------------
    // Operaciones atomicas de verdad (varios nucleos a la vez): LDXR/STXR, CAS, LDADD...
    // Si [addr, addr+sizeof(T)) esta alineado (siempre en ARM para estas instrucciones)
    // se hace con una instruccion atomica del PC sobre la memoria emulada.
    // ---------------------------------------------------------------------
    template <typename T>
    bool CompareExchange(VAddr addr, T& expected, T desired) {
        if (addr % sizeof(T) || addr >= ADDRESS_SPACE) [[unlikely]] {
            std::lock_guard lock(m_slowAtomicMutex);   // desalineado: sin garantias, como mucho entre si
            const T v = Read<T>(addr);
            if (v != expected) { expected = v; return false; }
            Write<T>(addr, desired);
            return true;
        }
        Page* page = GetOrCreatePage(addr / PAGE_SIZE);
        std::atomic_ref<T> ref(*reinterpret_cast<T*>(page->bytes.data() + (addr & (PAGE_SIZE - 1))));
        if (!ref.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) return false;
        if (page->code.load(std::memory_order_relaxed)) [[unlikely]] NoteCodeWrite(page, addr);
        return true;
    }
    // 16 bytes (STXP de dos X, CASP): sin instruccion de 128 bits portable; con candado
    bool CompareExchange128(VAddr addr, u64& lo, u64& hi, u64 new_lo, u64 new_hi) {
        std::lock_guard lock(m_slowAtomicMutex);
        const u64 vl = Read<u64>(addr), vh = Read<u64>(addr + 8);
        if (vl != lo || vh != hi) { lo = vl; hi = vh; return false; }
        Write<u64>(addr, new_lo);
        Write<u64>(addr + 8, new_hi);
        return true;
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
    size_t AllocatedPages() const { std::lock_guard lock(m_pageMutex); return m_pages.size(); }
    u64    AllocatedBytes() const { return static_cast<u64>(AllocatedPages()) * PAGE_SIZE; }

    // Lectura/escritura de un valor. Camino rapido: el valor cabe entero en una pagina
    // (casi siempre). Si cruza el borde entre dos paginas, va por ReadBytes/WriteBytes.
    // Un entero alineado se lee/escribe "de una vez" (atomico relajado: en el PC es la
    // misma instruccion de siempre). Asi, con varios nucleos, un valor nunca se ve a medias
    // y convive bien con los CompareExchange de otro nucleo, como en la CPU real.
    template <typename T>
    T Read(VAddr addr) {
        T value{};
        const u64 offset = addr & (PAGE_SIZE - 1);
        if (offset + sizeof(T) <= PAGE_SIZE && addr < ADDRESS_SPACE) {
            if (Page* page = FindPage(addr / PAGE_SIZE)) {
                u8* p = page->bytes.data() + offset;
                if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
                    if ((addr & (sizeof(T) - 1)) == 0)
                        return std::atomic_ref<T>(*reinterpret_cast<T*>(p)).load(std::memory_order_relaxed);
                }
                std::memcpy(&value, p, sizeof(T));
            }
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
            if (page->code.load(std::memory_order_relaxed)) [[unlikely]] NoteCodeWrite(page, addr);
            u8* p = page->bytes.data() + offset;
            if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
                if ((addr & (sizeof(T) - 1)) == 0) {
                    std::atomic_ref<T>(*reinterpret_cast<T*>(p)).store(value, std::memory_order_relaxed);
                    return;
                }
            }
            std::memcpy(p, &value, sizeof(T));
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

    // Crea las paginas de [addr, addr+size) que aun no existan (a cero), sin escribir nada
    void TouchPages(VAddr addr, u64 size) {
        if (!size || addr >= ADDRESS_SPACE) return;
        const u64 last = std::min<u64>(addr + size - 1, ADDRESS_SPACE - 1) / PAGE_SIZE;
        for (u64 i = addr / PAGE_SIZE; i <= last; ++i) GetOrCreatePage(i);
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
            if (page->code.load(std::memory_order_relaxed)) [[unlikely]] NoteCodeWrite(page, addr);
            std::memcpy(page->bytes.data() + offset, in, chunk);
            addr += chunk; in += chunk; size -= chunk;
        }
    }

private:
    struct Page {
        alignas(64) std::array<u8, PAGE_SIZE> bytes{};   // los 4 KB (a cero al crearla)
        std::atomic<bool> code{false};       // la CPU tiene instrucciones decodificadas de aqui
    };

    // Entrada de la tabla plana del JIT (el JIT la lee desde otro hilo: escritura atomica)
    void SetFlat(u64 index, void* value) {
        if (m_flat) std::atomic_ref<void*>(m_flat[index]).store(value, std::memory_order_release);
    }

    // Primera escritura en una pagina de codigo: se apunta y se desmarca (las
    // siguientes escrituras ya no cuentan hasta que la CPU la vuelva a decodificar).
    void NoteCodeWrite(Page* page, VAddr addr) {
        std::lock_guard lock(m_codeMutex);
        if (!page->code.exchange(false, std::memory_order_relaxed)) return;   // otro hilo ya lo hizo
        SetFlat(addr / PAGE_SIZE, page->bytes.data());   // ya no es codigo traducido
        const u64 n = m_codeWrites.load(std::memory_order_relaxed);
        m_ring[n % m_ring.size()] = addr & ~(PAGE_SIZE - 1);
        m_codeWrites.store(n + 1, std::memory_order_release);
        for (auto* flag : m_codeWriteFlags) flag->store(true, std::memory_order_relaxed);
    }

    // Tabla de paginas de dos niveles (como la de una CPU real):
    //   nivel 1: un hueco por cada 2 MB  (12 GB / 2 MB = 6144 huecos)
    //   nivel 2: 512 punteros a paginas de 4 KB, creado solo si hace falta
    // Buscar una pagina son dos accesos a array, mucho mas rapido que un unordered_map.
    static constexpr u64 L2_BITS = 9;                       // 512 paginas = 2 MB
    static constexpr u64 L2_SIZE = 1ull << L2_BITS;
    static constexpr u64 L1_SIZE = (ADDRESS_SPACE / PAGE_SIZE) >> L2_BITS;
    using L2Table = std::array<std::atomic<Page*>, L2_SIZE>;

    Page* FindPage(u64 index) const {
        const L2Table* l2 = m_table[index >> L2_BITS].load(std::memory_order_acquire);
        return l2 ? (*l2)[index & (L2_SIZE - 1)].load(std::memory_order_acquire) : nullptr;
    }

    Page* GetOrCreatePage(u64 index) {
        if (Page* p = FindPage(index)) [[likely]] return p;
        // Camino lento: con candado (dos hilos podrian querer crear la misma pagina)
        std::lock_guard lock(m_pageMutex);
        auto& l1 = m_table[index >> L2_BITS];
        L2Table* l2 = l1.load(std::memory_order_relaxed);
        if (!l2) {
            m_l2Tables.push_back(std::make_unique<L2Table>()); // todos los punteros a nullptr
            l2 = m_l2Tables.back().get();
            l1.store(l2, std::memory_order_release);
        }
        auto& slot = (*l2)[index & (L2_SIZE - 1)];
        Page* page = slot.load(std::memory_order_relaxed);
        if (!page) {
            m_pages.push_back(std::make_unique<Page>()); // página nueva inicializada a 0
            page = m_pages.back().get();
            slot.store(page, std::memory_order_release);
            SetFlat(index, page->bytes.data());
        }
        return page;
    }

    mutable std::mutex m_pageMutex;             // crear paginas
    std::mutex m_codeMutex;                     // apuntar escrituras en codigo
    std::vector<std::unique_ptr<Page>> m_pages;                      // duenas de las paginas
    std::vector<std::unique_ptr<L2Table>> m_l2Tables;                // duenas de las tablas de nivel 2
    std::array<std::atomic<L2Table*>, L1_SIZE> m_table{};
    u64 m_generation = 0;
    std::atomic<u64> m_codeWrites{0};           // escrituras en paginas de codigo
    std::array<VAddr, 64> m_ring{};             // que paginas eran (las ultimas 64)
    std::vector<std::atomic<bool>*> m_codeWriteFlags;   // una por CPU
    std::mutex m_slowAtomicMutex;
    std::unique_ptr<void*[]> m_flat;            // tabla plana para el JIT (solo si se pide)
    std::map<VAddr, MemoryRegion> m_regions; // ordenadas por direccion
};

} // namespace NeXo2::Core
