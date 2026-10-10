#pragma once
#include <map>
#include <mutex>
#include <vector>
#include "common/types.hpp"
#include "memory.hpp"

// Pantalla emulada: memoria de la GPU (nvmap) y la ultima imagen presentada.
//
// Flujo de una imagen (ver docs/07-nexo-internals/display.md):
//   1. El programa reserva memoria para sus buffers con nvdrv (/dev/nvmap).
//   2. Registra cada buffer en la cola del binder (SET_PREALLOCATED_BUFFER).
//   3. Dibuja en uno y lo entrega (QUEUE_BUFFER) -> Display::Present().
//   4. Present() lee el buffer de la memoria emulada, lo pasa de "block linear"
//      (el formato en bloques de la GPU de NVIDIA) a lineal y lo convierte a RGBA.
//   5. La interfaz (main.cpp) dibuja esa imagen en la ventana "Pantalla".
namespace NeXo2::HLE {

// Un bloque de memoria registrado en /dev/nvmap
struct NvMapObject {
    u32 handle = 0;      // numero que usa el programa con este fd
    u32 id = 0;          // id global (lo que viaja en el GraphicBuffer)
    u32 size = 0;
    u32 align = 0;
    u8  kind = 0;
    u64 address = 0;     // direccion en la memoria del programa (tras NVMAP_IOC_ALLOC)
};

class NvMapTable {
public:
    NvMapObject& Create(u32 size) {
        NvMapObject obj;
        obj.handle = m_next++;
        obj.id = obj.handle;          // usamos el mismo numero para handle e id
        obj.size = size;
        return m_objects[obj.handle] = obj;
    }
    NvMapObject* FindByHandle(u32 handle) {
        auto it = m_objects.find(handle);
        return it == m_objects.end() ? nullptr : &it->second;
    }
    NvMapObject* FindById(u32 id) { return FindByHandle(id); }
    void Free(u32 handle) { m_objects.erase(handle); }
    void Clear() { m_objects.clear(); m_next = 1; }

private:
    std::map<u32, NvMapObject> m_objects;
    u32 m_next = 1;
};

// Descripcion de un buffer de imagen (lo esencial del GraphicBuffer de Android/NVIDIA)
struct GraphicBufferInfo {
    u32 nvmap_id = 0;
    u32 width = 0, height = 0;
    u32 format = 0;          // PIXEL_FORMAT_* de Android (1 = RGBA8888, 4 = RGB565...)
    u32 layout = 0;          // 1 = pitch (lineal), 3 = block linear
    u32 pitch = 0;           // bytes por fila
    u32 offset = 0;          // desde el inicio del bloque nvmap
    u32 block_height_log2 = 0;
    u32 size = 0;
};

// Ultima imagen presentada, ya en RGBA (8 bits por canal, R primero en memoria)
struct DisplayFrame {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> rgba;
    u64 count = 0;           // cuantas imagenes se han presentado (para saber si hay una nueva)
};

class Display {
public:
    static constexpr u32 WIDTH = 1280;   // resolucion del modo portatil
    static constexpr u32 HEIGHT = 720;

    NvMapTable& NvMap() { return m_nvmap; }
    // La imagen la escribe el hilo de la GPU (Present va en su cola): quien la lea desde
    // otro hilo (la interfaz) tiene que coger LockFrame() mientras la usa.
    const DisplayFrame& Frame() const { return m_frame; }
    std::unique_lock<std::mutex> LockFrame() const { return std::unique_lock(m_frameMutex); }

    // Copia el buffer 'buf' de la memoria del programa a la imagen de salida.
    // Devuelve false si el buffer no se puede interpretar.
    bool Present(Core::Memory& memory, const GraphicBufferInfo& buf);
    // Igual, con la direccion del bloque nvmap ya resuelta (para la cola de la GPU:
    // la tabla nvmap es del hilo de la emulacion)
    bool PresentAt(Core::Memory& memory, const GraphicBufferInfo& buf, u64 nvmap_address);

    void Reset() { m_nvmap.Clear(); std::lock_guard lock(m_frameMutex); m_frame = DisplayFrame{}; }

private:
    NvMapTable   m_nvmap;
    mutable std::mutex m_frameMutex;
    DisplayFrame m_frame;
};

// Convierte una imagen "block linear" (GOBs de 64x8 bytes en bloques de 2^n GOBs de alto)
// a lineal con 'pitch' bytes por fila. Es la inversa de lo que hace libnx en framebufferEnd().
void DeswizzleBlockLinear(const u8* in, u8* out, u32 pitch, u32 height, u32 block_height_log2);

} // namespace NeXo2::HLE
