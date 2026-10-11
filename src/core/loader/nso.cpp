#include "nso.hpp"
#include <cstring>

namespace NeXo2::Loader {

using Core::MemoryPermission;
using Core::MemoryState;

namespace {
constexpr u32 NSO_MAGIC = 0x304F534E;   // "NSO0"
constexpr u64 PAGE = 0x1000;
constexpr u64 AlignUp(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
} // namespace

// Formato de un bloque LZ4: secuencias de [token][literales][offset][longitud extra].
//   token: 4 bits altos = numero de literales, 4 bajos = longitud de la copia - 4
//   (15 = sigue en los bytes siguientes, sumando hasta encontrar uno que no sea 255)
//   offset: 2 bytes, cuanto hay que mirar hacia atras en lo ya escrito
// La ultima secuencia solo tiene literales.
bool Lz4DecompressBlock(const u8* src, size_t src_size, u8* dst, size_t dst_size) {
    size_t ip = 0, op = 0;
    auto extra = [&](size_t& len) {
        u8 b;
        do {
            if (ip >= src_size) return false;
            b = src[ip++];
            len += b;
        } while (b == 255);
        return true;
    };
    while (ip < src_size) {
        const u8 token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15 && !extra(lit)) return false;
        if (lit > src_size - ip || lit > dst_size - op) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip == src_size) break;                     // ultima secuencia
        if (src_size - ip < 2) return false;
        const size_t offset = size_t(src[ip]) | (size_t(src[ip + 1]) << 8);
        ip += 2;
        if (offset == 0 || offset > op) return false;
        size_t len = token & 15;
        if (len == 15 && !extra(len)) return false;
        len += 4;
        if (len > dst_size - op) return false;
        for (size_t i = 0; i < len; ++i) dst[op + i] = dst[op - offset + i];   // puede solaparse
        op += len;
    }
    return op == dst_size;
}

NsoLoadResult LoadNso(const std::vector<u8>& file, Core::Memory& memory, u64 base, const std::string& name) {
    NsoLoadResult result;
    auto fail = [&](const std::string& why) { result.error = name + ": " + why; return result; };

    NsoHeader h{};
    if (file.size() < sizeof(h)) return fail("archivo demasiado pequeno para ser un NSO");
    std::memcpy(&h, file.data(), sizeof(h));
    if (h.magic != NSO_MAGIC) return fail("no es un NSO (falta el magic \"NSO0\")");
    if (base % PAGE) return fail("la direccion de carga debe estar alineada a 4 KB");

    const NsoSegment segs[3] = {h.text, h.ro, h.data};
    const u32 file_sizes[3] = {h.text_file_size, h.ro_file_size, h.data_file_size};
    for (int i = 0; i < 3; ++i) {
        const NsoSegment& s = segs[i];
        const bool compressed = (h.flags >> i) & 1;
        const u64 stored = compressed ? file_sizes[i] : s.size;
        if (u64(s.file_offset) + stored > file.size()) return fail("un segmento se sale del archivo (esta cortado?)");
        if (s.memory_offset % PAGE) return fail("segmento no alineado a 4 KB en memoria");
        std::vector<u8> out(s.size);
        if (compressed) {
            if (!Lz4DecompressBlock(file.data() + s.file_offset, stored, out.data(), out.size()))
                return fail("datos LZ4 no validos");
        } else if (s.size) {
            std::memcpy(out.data(), file.data() + s.file_offset, s.size);
        }
        memory.WriteBytes(base + s.memory_offset, out.data(), out.size());
    }
    if (h.ro.memory_offset < u64(h.text.memory_offset) + h.text.size ||
        h.data.memory_offset < u64(h.ro.memory_offset) + h.ro.size)
        return fail("segmentos solapados o desordenados");

    // .bss: justo despues de .data, a cero
    const u64 data_end = u64(h.data.memory_offset) + h.data.size;
    const u64 image_end = AlignUp(data_end + h.bss_size, PAGE);
    if (image_end > data_end) {
        const std::vector<u8> zeros(image_end - data_end, 0);
        memory.WriteBytes(base + data_end, zeros.data(), zeros.size());
    }

    memory.MapRegion(base + h.text.memory_offset, AlignUp(h.text.size, PAGE), MemoryState::Code,
                     MemoryPermission::ReadExecute, name + " .text");
    memory.MapRegion(base + h.ro.memory_offset, AlignUp(h.ro.size, PAGE), MemoryState::Code,
                     MemoryPermission::Read, name + " .rodata");
    memory.MapRegion(base + h.data.memory_offset, image_end - h.data.memory_offset, MemoryState::CodeData,
                     MemoryPermission::ReadWrite, name + " .data + .bss");
    result.image_size = image_end;
    result.ok = true;
    return result;
}

} // namespace NeXo2::Loader
