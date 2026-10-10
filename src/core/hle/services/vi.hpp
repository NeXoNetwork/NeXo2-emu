#pragma once
#include <array>
#include <map>
#include <memory>
#include <vector>
#include "hle/display.hpp"
#include "hle/service.hpp"

// "vi:m" / "vi:s" / "vi:u": Visual Interface, el servicio de pantallas y capas.
// Referencia: https://switchbrew.org/wiki/Display_services
//
//   vi:m -> GetDisplayService -> IApplicationDisplayService
//              ├─ 100  IHOSBinderDriverRelay   (cola de buffers estilo Android)
//              ├─ 101  ISystemDisplayService
//              ├─ 102  IManagerDisplayService
//              ├─ 1010 OpenDisplay("Default")
//              └─ 2020 OpenLayer / 2030 CreateStrayLayer -> "ventana nativa" con un binder
//
// El binder habla el protocolo IGraphicBufferProducer de Android (parcels):
//   CONNECT, SET_PREALLOCATED_BUFFER, DEQUEUE_BUFFER, REQUEST_BUFFER, QUEUE_BUFFER...
// QUEUE_BUFFER es "muestra esta imagen": llamamos a Display::Present().
namespace NeXo2::HLE {

// Una cola de buffers (lo que hay detras de cada capa)
struct BufferQueue {
    struct Slot {
        bool configured = false;
        bool dequeued = false;
        GraphicBufferInfo info;
        u64  present_ticket = 0;   // tarea de la GPU que lo presenta (hay que esperarla al reusarlo)
    };
    std::array<Slot, 64> slots;
    u32  next = 0;          // siguiente hueco a entregar (por turnos)
    bool connected = false;
};

// Estado compartido por todas las interfaces de una sesion de vi
struct ViState {
    std::map<u32, BufferQueue> queues;   // id del binder -> cola
    u64 next_layer_id = 1;
};

class ViRoot final : public ServiceObject {
public:
    ViRoot(std::string name, u32 get_display_service_cmd);
};

class ApplicationDisplayService final : public ServiceObject {
public:
    explicit ApplicationDisplayService(std::shared_ptr<ViState> state);
private:
    void OpenLayer(IpcContext& ctx);
    void CreateStrayLayer(IpcContext& ctx);
    // Escribe la "ventana nativa" (un parcel con el id del binder) en el buffer de salida
    u64  WriteNativeWindow(IpcContext& ctx, u64 layer_id);
    std::shared_ptr<ViState> m_state;
};

class SystemDisplayService final : public ServiceObject {
public:
    SystemDisplayService();
};

class ManagerDisplayService final : public ServiceObject {
public:
    explicit ManagerDisplayService(std::shared_ptr<ViState> state);
private:
    std::shared_ptr<ViState> m_state;
};

class HosBinderDriver final : public ServiceObject {
public:
    explicit HosBinderDriver(std::shared_ptr<ViState> state);
private:
    void TransactParcel(IpcContext& ctx);
    std::vector<u8> Transact(IpcContext& ctx, BufferQueue& queue, u32 code, const std::vector<u8>& parcel);
    std::shared_ptr<ViState> m_state;
};

// --- Parcels de Android (formato de los mensajes del binder) ---
class ParcelReader {
public:
    explicit ParcelReader(const std::vector<u8>& raw);
    s32  ReadI32();
    void ReadInterfaceToken();                       // i32 politica + String16 con el nombre
    std::vector<u8> ReadFlattened();                 // i32 longitud, i32 n_fds, datos
    bool Ok() const { return m_ok; }
private:
    std::vector<u8> m_payload;
    size_t m_pos = 0;
    bool m_ok = true;
};

class ParcelWriter {
public:
    void WriteI32(s32 v);
    void WriteBytes(const void* data, size_t size);  // con relleno a 4 bytes
    void WriteFlattened(const void* data, size_t size);
    std::vector<u8> Serialize() const;               // cabecera (16 bytes) + datos
private:
    std::vector<u8> m_payload;
};

// Extrae lo esencial de un GraphicBuffer serializado (magic "GBFR")
bool ParseGraphicBuffer(const std::vector<u8>& flat, GraphicBufferInfo& out);

} // namespace NeXo2::HLE
