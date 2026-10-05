#pragma once
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "common/types.hpp"
#include "memory.hpp"

// Mensajes IPC de Horizon (HIPC) y los dos protocolos que van encima:
//   CMIF: el normal. Cabecera "SFCI" (peticion) / "SFCO" (respuesta), dominios.
//   TIPC: el "ligero" (sm: en firmwares nuevos). Sin cabecera, el resultado va primero.
//
// Formato completo: docs/03-services-ipc/hipc.md y switchbrew.org/wiki/HIPC
// Este codigo sigue el mismo formato que usa libnx (nx/include/switch/sf/).
namespace NeXo2::HLE {

class Kernel;
class ServiceObject;
struct SessionState;

// Tipo de mensaje (campo 'type' de la cabecera HIPC)
namespace CommandType {
    constexpr u32 Close              = 2;
    constexpr u32 Request            = 4;
    constexpr u32 Control            = 5;
    constexpr u32 RequestWithContext = 6;
    constexpr u32 ControlWithContext = 7;
    constexpr u32 TipcBase           = 16; // TIPC: type = 16 + id del comando
}

constexpr u32 CMIF_IN_MAGIC  = 0x49434653; // "SFCI"
constexpr u32 CMIF_OUT_MAGIC = 0x4F434653; // "SFCO"
constexpr u32 IPC_BUFFER_SIZE = 0x100;      // el mensaje cabe en los primeros 0x100 bytes de la TLS

// Un buffer de memoria que el programa comparte con el servicio
struct IpcBuffer {
    u64 address = 0;
    u64 size = 0;
};

// Peticion ya leida de memoria
struct IpcRequest {
    u32 type = 0;

    bool has_pid = false;
    u64  pid = 0;
    std::vector<u32> copy_handles;
    std::vector<u32> move_handles;

    std::vector<IpcBuffer> x_buffers;  // "send statics" (punteros de entrada, tipo X)
    std::vector<IpcBuffer> a_buffers;  // buffers de entrada (tipo A)
    std::vector<IpcBuffer> b_buffers;  // buffers de salida (tipo B)
    std::vector<IpcBuffer> w_buffers;  // buffers de entrada/salida (tipo W)
    std::vector<IpcBuffer> c_buffers;  // "receive list" (punteros de salida, tipo C)

    u64 data_offset = 0;      // donde empiezan las palabras de datos (desde el inicio del mensaje)
    u32 data_size = 0;        // tamano de los datos en bytes

    // Solo CMIF/TIPC (rellenado por el despachador)
    bool is_domain_message = false;
    u8   domain_command = 0;  // 1 = enviar mensaje, 2 = cerrar objeto
    u32  domain_object_id = 0;
    std::vector<u32> domain_in_objects;
    u32  command_id = 0;
    u64  payload_offset = 0;  // argumentos del comando (tras SFCI o desde los datos en TIPC)
    u32  payload_size = 0;
};

// Lee la cabecera HIPC y todos los descriptores de un mensaje en 'base'.
IpcRequest ParseHipcRequest(Core::Memory& memory, u64 base);

// Contexto de una llamada: lo que ve un servicio al atender un comando.
// Sirve para leer los argumentos y preparar la respuesta.
class IpcContext {
public:
    enum class Protocol { Cmif, Tipc };

    IpcContext(Kernel& kernel, Core::Memory& memory, IpcRequest request, Protocol protocol,
               bool session_is_domain);

    Kernel&       GetKernel() { return m_kernel; }
    Core::Memory& GetMemory() { return m_memory; }
    const IpcRequest& Request() const { return m_request; }
    u32 CommandId() const { return m_request.command_id; }
    Protocol GetProtocol() const { return m_protocol; }

    // --- Argumentos de entrada (en orden, como estan en el mensaje) ---
    template <typename T>
    T Pop() {
        T value{};
        PopBytes(&value, sizeof(T));
        return value;
    }
    void PopBytes(void* out, size_t size);
    void SkipBytes(size_t size) { m_popOffset += size; }
    void AlignPop(size_t alignment) { m_popOffset = (m_popOffset + alignment - 1) & ~(alignment - 1); }

    u64 Pid() const { return m_request.pid; }

    // Buffer de entrada numero 'index' (A o, si esta vacio, X)
    std::vector<u8> ReadBuffer(size_t index = 0) const;
    // Escribe en el buffer de salida 'index' (B o, si esta vacio, C). Devuelve bytes escritos.
    size_t WriteBuffer(const void* data, size_t size, size_t index = 0);
    u64 GetWriteBufferSize(size_t index = 0) const;

    // --- Respuesta ---
    void SetResult(u32 result) { m_result = result; }
    u32  GetResult() const { return m_result; }

    template <typename T>
    void Push(const T& value) { PushBytes(&value, sizeof(T)); }
    void PushBytes(const void* data, size_t size);

    // Devuelve un objeto de servicio nuevo al programa. En un dominio va como id
    // de objeto; si no, como un handle de sesion nuevo.
    void PushInterface(std::shared_ptr<ServiceObject> object);
    void PushMoveHandle(u32 handle) { m_moveHandles.push_back(handle); }
    void PushCopyHandle(u32 handle) { m_copyHandles.push_back(handle); }

    // El comando no esta implementado: para la CPU con un mensaje claro.
    void Unimplemented(const std::string& service_name);

    // Escribe la respuesta en 'base' (normalmente la TLS del hilo).
    // 'session' hace falta para registrar interfaces nuevas en el dominio.
    bool WriteResponse(u64 base, const std::shared_ptr<SessionState>& session);

    bool IsHandled() const { return !m_unimplemented; }

private:
    Kernel&       m_kernel;
    Core::Memory& m_memory;
    IpcRequest    m_request;
    Protocol      m_protocol;
    bool          m_sessionIsDomain;
    size_t        m_popOffset = 0;

    u32 m_result = 0;
    std::vector<u8>  m_payload;
    std::vector<u32> m_copyHandles;
    std::vector<u32> m_moveHandles;
    std::vector<std::shared_ptr<ServiceObject>> m_outObjects;
    bool m_unimplemented = false;
};

// Convierte el nombre de servicio empaquetado en un u64 ("set:sys\0") en texto
std::string ServiceNameFromU64(u64 packed);

} // namespace NeXo2::HLE
