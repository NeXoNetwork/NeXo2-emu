#pragma once
// GPU emulada (Maxwell GM20B, la de la Switch 1, que la Switch 2 ejecuta en modo
// compatibilidad y que usa todo el homebrew). Fase 1: sin shaders.
//
//   Gpu                 la GPU entera: memoria virtual de la GPU, syncpoints, canales
//   GpuMemoryManager    direcciones de la GPU (40 bits) -> memoria del programa
//   Channel             un canal (/dev/nvhost-gpu): lee el GPFIFO y los pushbuffers,
//                       y reparte los "metodos" entre los motores (engines.hpp)
//
// Todo se ejecuta al momento (sincrono), dentro del ioctl que envia el trabajo: cuando
// el programa recibe el fence, el trabajo ya esta hecho. Ver docs/07-nexo-internals/gpu.md.
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/types.hpp"

namespace NeXo2::Core { class Memory; }

namespace NeXo2::GPU {

class Maxwell3D;
class MaxwellDma;
class Fermi2D;
class KeplerCompute;
class InlineToMemory;

// ----------------------------------------------------------------------------
// Memoria virtual de la GPU
// ----------------------------------------------------------------------------
class GpuMemoryManager {
public:
    // Regiones de direcciones (las que devuelve GET_VA_REGIONS en la consola)
    static constexpr u64 SMALL_PAGE = 0x1000;
    static constexpr u64 BIG_PAGE = 0x10000;
    static constexpr u64 SMALL_REGION_START = 0x0000'0400'0000;   // paginas de 4 KB
    static constexpr u64 SMALL_REGION_END   = 0x0003'FC00'0000;
    static constexpr u64 BIG_REGION_START   = 0x0004'0000'0000;   // paginas de 64 KB
    static constexpr u64 BIG_REGION_END     = 0x00FF'FFFF'0000;   // (40 bits)

    explicit GpuMemoryManager(Core::Memory& memory) : m_memory(memory) {}
    void Reset() { m_reserved.clear(); m_maps.clear(); }

    // Reserva un hueco de 'size' bytes (alineado a 'align') y devuelve su direccion; 0 = no hay
    u64  Allocate(u64 size, u64 align, bool big_pages);
    bool ReserveFixed(u64 va, u64 size);
    void Free(u64 va, u64 size);

    // Hace que [va, va+size) apunte a la memoria del programa en 'cpu_addr'
    void Map(u64 va, u64 cpu_addr, u64 size);
    // Quita la proyeccion que empieza en 'va'; devuelve su tamano (0 si no habia)
    u64  Unmap(u64 va);
    // Direccion de la memoria del programa para una direccion de la GPU
    std::optional<u64> Translate(u64 va) const;

    void ReadBlock(u64 va, void* dst, size_t size) const;
    void WriteBlock(u64 va, const void* src, size_t size);
    template <typename T> T Read(u64 va) const { T v{}; ReadBlock(va, &v, sizeof(T)); return v; }
    template <typename T> void Write(u64 va, T v) { WriteBlock(va, &v, sizeof(T)); }

    size_t MappingCount() const { return m_maps.size(); }

private:
    struct Mapping { u64 size; u64 cpu; };
    // Busca la proyeccion que contiene 'va'
    const std::pair<const u64, Mapping>* Find(u64 va) const;

    Core::Memory& m_memory;
    std::map<u64, u64> m_reserved;        // inicio -> tamano (espacio de direcciones ocupado)
    std::map<u64, Mapping> m_maps;        // inicio -> proyeccion
};

// ----------------------------------------------------------------------------
// Syncpoints: contadores que la GPU incrementa al terminar trabajo. Los fences
// de NVIDIA son (syncpoint, valor): "el trabajo estara hecho cuando el syncpoint
// llegue a este valor".
// ----------------------------------------------------------------------------
class Syncpoints {
public:
    static constexpr u32 COUNT = 192;
    u32  Read(u32 id) const { return id < COUNT ? m_values[id] : 0; }
    void Increment(u32 id);
    bool Reached(u32 id, u32 threshold) const { return s32(Read(id) - threshold) >= 0; }
    // Cada canal recibe un syncpoint propio
    u32  AllocateSyncpoint() { return m_next < COUNT ? m_next++ : COUNT - 1; }
    // Llama a 'fire' cuando el syncpoint llegue a 'threshold' (ya mismo si ha llegado).
    // Asi nvhost-ctrl activa el evento por el que espera el programa.
    void AddWaiter(u32 id, u32 threshold, std::function<void()> fire);
    void Reset() { m_values.fill(0); m_next = 1; m_waiters.clear(); }
private:
    struct Waiter { u32 id, threshold; std::function<void()> fire; };
    std::array<u32, COUNT> m_values{};
    u32 m_next = 1;   // el 0 queda libre (nadie lo usa)
    std::vector<Waiter> m_waiters;
};

class Gpu;

// Un "motor" de la GPU (3D, copia, 2D...). Recibe metodos: escrituras en sus registros.
class Engine {
public:
    virtual ~Engine() = default;
    // 'last' = es la ultima palabra de ese paquete del pushbuffer (las macros se ejecutan ahi)
    virtual void CallMethod(u32 method, u32 arg, bool last) = 0;
    virtual const char* Name() const = 0;
};

// ----------------------------------------------------------------------------
// Canal: lo que el programa abre como /dev/nvhost-gpu
// ----------------------------------------------------------------------------
class Channel {
public:
    Channel(Gpu& gpu, u32 id);
    ~Channel();

    u32 Id() const { return m_id; }
    u32 SyncpointId() const { return m_syncpoint; }

    // Entradas GPFIFO: cada una apunta a una lista de comandos (pushbuffer)
    void SubmitGpfifo(const std::vector<u64>& entries);

    // Para los tests: procesar un pushbuffer que esta en memoria de la GPU
    void ProcessPushbuffer(u64 va, u32 words);

    Maxwell3D& Get3D() { return *m_3d; }

private:
    void CallMethod(u32 subchannel, u32 method, u32 arg, bool last);
    void HostMethod(u32 method, u32 arg);
    void BindSubchannel(u32 subchannel, u32 class_id);

    Gpu& m_gpu;
    u32 m_id;
    u32 m_syncpoint;
    std::unique_ptr<Maxwell3D> m_3d;
    std::unique_ptr<MaxwellDma> m_dma;
    std::unique_ptr<Fermi2D> m_2d;
    std::unique_ptr<KeplerCompute> m_compute;
    std::unique_ptr<InlineToMemory> m_inline;
    std::array<Engine*, 8> m_subchannels{};
    // Registros del propio canal (metodos 0x00..0x3F: semaforos, syncpoints...)
    std::array<u32, 0x40> m_host{};
};

// ----------------------------------------------------------------------------
// La GPU
// ----------------------------------------------------------------------------
class Gpu {
public:
    explicit Gpu(Core::Memory& memory);
    ~Gpu();

    GpuMemoryManager& MemoryManager() { return m_mm; }
    Syncpoints& GetSyncpoints() { return m_syncpoints; }
    Core::Memory& CpuMemory() { return m_memory; }

    Channel& CreateChannel();
    Channel* GetChannel(u32 id);

    void Reset();

    // Estadisticas para la interfaz y los tests
    struct Stats {
        u64 submits = 0;          // envios GPFIFO
        u64 methods = 0;          // metodos procesados
        u64 clears = 0;           // borrados de render target
        u64 copies = 0;           // copias (DMA, 2D, inline)
        u64 macros = 0;           // macros ejecutadas
        u64 draws_skipped = 0;    // dibujos ignorados (sin shaders todavia)
        u64 unknown_methods = 0;
    };
    Stats& GetStats() { return m_stats; }

    // Mensajes de aviso (una vez por tipo): la interfaz y el log los muestran
    void Warn(const std::string& key, const std::string& message);

private:
    Core::Memory& m_memory;
    GpuMemoryManager m_mm;
    Syncpoints m_syncpoints;
    std::vector<std::unique_ptr<Channel>> m_channels;
    Stats m_stats;
    std::map<std::string, bool> m_warned;
};

} // namespace NeXo2::GPU
