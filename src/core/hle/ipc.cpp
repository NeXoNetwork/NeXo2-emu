#include "ipc.hpp"
#include "kernel.hpp"
#include "kernel_objects.hpp"
#include "service.hpp"
#include "common/logger.hpp"
#include <algorithm>

namespace NeXo2::HLE {

// ============================================================================
//  Lectura de la peticion (cabecera HIPC + descriptores)
// ============================================================================

IpcRequest ParseHipcRequest(Core::Memory& m, u64 base) {
    IpcRequest r;
    const u32 h0 = m.Read<u32>(base + 0);
    const u32 h1 = m.Read<u32>(base + 4);
    u64 off = 8;

    // Cabecera (2 palabras)
    r.type = h0 & 0xFFFF;
    const u32 num_x = (h0 >> 16) & 0xF;
    const u32 num_a = (h0 >> 20) & 0xF;
    const u32 num_b = (h0 >> 24) & 0xF;
    const u32 num_w = (h0 >> 28) & 0xF;
    const u32 num_data_words   = h1 & 0x3FF;
    const u32 recv_static_mode = (h1 >> 10) & 0xF;
    const bool has_special     = (h1 >> 31) & 1;

    // Cabecera especial: PID y handles que se envian
    if (has_special) {
        const u32 sh = m.Read<u32>(base + off);
        off += 4;
        r.has_pid = sh & 1;
        const u32 num_copy = (sh >> 1) & 0xF;
        const u32 num_move = (sh >> 5) & 0xF;
        if (r.has_pid) { r.pid = m.Read<u64>(base + off); off += 8; }
        for (u32 i = 0; i < num_copy; ++i, off += 4) r.copy_handles.push_back(m.Read<u32>(base + off));
        for (u32 i = 0; i < num_move; ++i, off += 4) r.move_handles.push_back(m.Read<u32>(base + off));
    }

    // Descriptores X (8 bytes): la direccion va repartida en varios trozos
    for (u32 i = 0; i < num_x; ++i, off += 8) {
        const u32 w0 = m.Read<u32>(base + off);
        const u32 w1 = m.Read<u32>(base + off + 4);
        IpcBuffer b;
        b.address = u64(w1) | (u64((w0 >> 12) & 0xF) << 32) | (u64((w0 >> 6) & 0x3F) << 36);
        b.size = w0 >> 16;
        r.x_buffers.push_back(b);
    }

    // Descriptores A, B y W (12 bytes cada uno)
    auto read_buffer = [&](std::vector<IpcBuffer>& out, u32 count) {
        for (u32 i = 0; i < count; ++i, off += 12) {
            const u32 size_low = m.Read<u32>(base + off);
            const u32 addr_low = m.Read<u32>(base + off + 4);
            const u32 w2       = m.Read<u32>(base + off + 8);
            IpcBuffer b;
            b.address = u64(addr_low) | (u64((w2 >> 28) & 0xF) << 32) | (u64((w2 >> 2) & 0x3FFFFF) << 36);
            b.size    = u64(size_low) | (u64((w2 >> 24) & 0xF) << 32);
            out.push_back(b);
        }
    };
    read_buffer(r.a_buffers, num_a);
    read_buffer(r.b_buffers, num_b);
    read_buffer(r.w_buffers, num_w);

    // Palabras de datos (aqui van las cabeceras CMIF/TIPC y los argumentos)
    r.data_offset = off;
    r.data_size = num_data_words * 4;
    off += r.data_size;

    // Lista de recepcion (descriptores C): justo despues de los datos.
    // Modo 0 = ninguno, 2 = uno "automatico" dentro del mensaje (no soportado), >2 = (modo - 2) entradas.
    const u32 num_c = recv_static_mode > 2 ? recv_static_mode - 2 : 0;
    for (u32 i = 0; i < num_c; ++i, off += 8) {
        const u32 w0 = m.Read<u32>(base + off);
        const u32 w1 = m.Read<u32>(base + off + 4);
        IpcBuffer b;
        b.address = u64(w0) | (u64(w1 & 0xFFFF) << 32);
        b.size = w1 >> 16;
        r.c_buffers.push_back(b);
    }
    return r;
}

// ============================================================================
//  Contexto de la llamada
// ============================================================================

IpcContext::IpcContext(Kernel& kernel, Core::Memory& memory, IpcRequest request, Protocol protocol,
                       bool session_is_domain)
    : m_kernel(kernel), m_memory(memory), m_request(std::move(request)), m_protocol(protocol),
      m_sessionIsDomain(session_is_domain) {}

void IpcContext::PopBytes(void* out, size_t size) {
    std::memset(out, 0, size);
    // No leer mas alla de los datos que mando el programa
    const size_t available = (m_popOffset < m_request.payload_size) ? m_request.payload_size - m_popOffset : 0;
    m_memory.ReadBytes(m_request.payload_offset + m_popOffset, out, std::min(size, available));
    m_popOffset += size;
}

std::vector<u8> IpcContext::ReadBuffer(size_t index) const {
    IpcBuffer buf;
    if (index < m_request.a_buffers.size() && m_request.a_buffers[index].size > 0) buf = m_request.a_buffers[index];
    else if (index < m_request.x_buffers.size())                                    buf = m_request.x_buffers[index];
    std::vector<u8> data(static_cast<size_t>(buf.size));
    if (!data.empty()) m_memory.ReadBytes(buf.address, data.data(), data.size());
    return data;
}

u64 IpcContext::GetWriteBufferSize(size_t index) const {
    if (index < m_request.b_buffers.size() && m_request.b_buffers[index].size > 0) return m_request.b_buffers[index].size;
    if (index < m_request.c_buffers.size()) return m_request.c_buffers[index].size;
    return 0;
}

size_t IpcContext::WriteBuffer(const void* data, size_t size, size_t index) {
    IpcBuffer buf;
    if (index < m_request.b_buffers.size() && m_request.b_buffers[index].size > 0) buf = m_request.b_buffers[index];
    else if (index < m_request.c_buffers.size())                                    buf = m_request.c_buffers[index];
    const size_t n = std::min<size_t>(size, static_cast<size_t>(buf.size));
    if (n > 0) m_memory.WriteBytes(buf.address, data, n);
    return n;
}

void IpcContext::PushBytes(const void* data, size_t size) {
    const u8* p = static_cast<const u8*>(data);
    m_payload.insert(m_payload.end(), p, p + size);
}

void IpcContext::PushInterface(std::shared_ptr<ServiceObject> object) {
    m_outObjects.push_back(std::move(object));
}

void IpcContext::Unsupported(const std::string& message) {
    m_unimplemented = true;
    m_kernel.HaltWithMessage(message);
}

void IpcContext::Unimplemented(const std::string& service_name) {
    m_unimplemented = true;
    m_kernel.ReportUnimplemented(service_name, m_request.command_id);
}

// ============================================================================
//  Escritura de la respuesta
// ============================================================================

bool IpcContext::WriteResponse(u64 base, const std::shared_ptr<SessionState>& session) {
    const bool cmif_domain = (m_protocol == Protocol::Cmif) && m_sessionIsDomain;

    // Interfaces nuevas: en un dominio se devuelven ids; si no, handles de sesion nuevos
    std::vector<u32> object_ids;
    std::vector<u32> object_handles;
    for (auto& obj : m_outObjects) {
        if (cmif_domain) object_ids.push_back(session->AddDomainObject(obj));
        else             object_handles.push_back(m_kernel.CreateSessionHandle(obj));
    }
    // Los handles de interfaces van antes que los demas handles "move" (como espera libnx)
    std::vector<u32> move_handles = object_handles;
    move_handles.insert(move_handles.end(), m_moveHandles.begin(), m_moveHandles.end());

    std::vector<u32> words;
    words.push_back(0); // cabecera 0 (tipo 0, sin buffers)
    words.push_back(0); // cabecera 1 (se rellena al final)

    const bool has_special = !m_copyHandles.empty() || !move_handles.empty();
    if (has_special) {
        words.push_back((u32(m_copyHandles.size()) << 1) | (u32(move_handles.size()) << 5));
        words.insert(words.end(), m_copyHandles.begin(), m_copyHandles.end());
        words.insert(words.end(), move_handles.begin(), move_handles.end());
    }
    const size_t data_start = words.size();

    // Argumentos de salida en palabras de 32 bits
    std::vector<u32> payload_words((m_payload.size() + 3) / 4, 0);
    if (!m_payload.empty()) std::memcpy(payload_words.data(), m_payload.data(), m_payload.size());

    if (m_protocol == Protocol::Cmif) {
        // Los datos CMIF empiezan alineados a 16 bytes desde el inicio del mensaje
        while ((words.size() * 4) % 16 != 0) words.push_back(0);
        if (cmif_domain) {
            words.push_back(u32(object_ids.size()));
            words.push_back(0); words.push_back(0); words.push_back(0);
        }
        words.push_back(CMIF_OUT_MAGIC);
        words.push_back(0);        // version
        words.push_back(m_result);
        words.push_back(0);        // token
        words.insert(words.end(), payload_words.begin(), payload_words.end());
        words.insert(words.end(), object_ids.begin(), object_ids.end());
    } else {
        // TIPC: primero el resultado, luego los datos
        words.push_back(m_result);
        words.insert(words.end(), payload_words.begin(), payload_words.end());
    }

    const u32 num_data_words = u32(words.size() - data_start);
    words[1] = (num_data_words & 0x3FF) | (has_special ? 0x80000000u : 0u);

    if (words.size() * 4 > IPC_BUFFER_SIZE) {
        Common::Logger::Log(Common::Logger::Level::Error, "[IPC] La respuesta no cabe en el buffer IPC");
        return false;
    }
    m_memory.WriteBytes(base, words.data(), words.size() * 4);
    return true;
}

std::string ServiceNameFromU64(u64 packed) {
    std::string name;
    for (int i = 0; i < 8; ++i) {
        const char c = static_cast<char>((packed >> (i * 8)) & 0xFF);
        if (c == '\0') break;
        name += c;
    }
    return name;
}

} // namespace NeXo2::HLE
