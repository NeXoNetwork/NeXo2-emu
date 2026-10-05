#include "nro.hpp"
#include <cstring>

namespace NeXo2::Loader {

using Core::MemoryPermission;
using Core::MemoryState;

namespace {

constexpr u32 NRO_MAGIC  = 0x304F524E; // "NRO0" en little endian
constexpr u32 ASET_MAGIC = 0x54455341; // "ASET"
constexpr u64 PAGE       = 0x1000;

constexpr u64 AlignUp(u64 value, u64 align) { return (value + align - 1) & ~(align - 1); }

// Lee un struct "plano" del archivo (sin pasarse del final).
template <typename T>
bool ReadAt(const std::vector<u8>& file, u64 offset, T& out) {
    if (offset + sizeof(T) > file.size()) return false;
    std::memcpy(&out, file.data() + offset, sizeof(T));
    return true;
}

// Cadena de tamano fijo terminada en 0 (como en el NACP).
std::string FixedString(const std::vector<u8>& file, u64 offset, u64 max_len) {
    std::string s;
    for (u64 i = 0; i < max_len && offset + i < file.size(); ++i) {
        const char c = static_cast<char>(file[offset + i]);
        if (c == '\0') break;
        s += c;
    }
    return s;
}

// Lee nombre y autor del NACP (bloque de assets "ASET" pegado al final del NRO).
// El NACP empieza con 16 idiomas, cada uno: nombre (0x200 bytes) + autor (0x100).
void ReadAssets(const std::vector<u8>& file, u64 asset_offset, NroInfo& info) {
    struct AssetSection { u64 offset, size; };
    struct AssetHeader { u32 magic, version; AssetSection icon, nacp, romfs; };
    AssetHeader aset{};
    if (!ReadAt(file, asset_offset, aset) || aset.magic != ASET_MAGIC) return;
    info.has_assets = true;
    if (aset.nacp.size < 0x3000) return;
    const u64 nacp = asset_offset + aset.nacp.offset;
    for (int lang = 0; lang < 16; ++lang) {
        const u64 entry = nacp + lang * 0x300;
        std::string title = FixedString(file, entry, 0x200);
        if (!title.empty()) {
            info.title  = title;
            info.author = FixedString(file, entry + 0x200, 0x100);
            return;
        }
    }
}

} // namespace

NroLoadResult LoadNro(const std::vector<u8>& file, Core::Memory& memory, u64 base) {
    NroLoadResult result;
    NroInfo& info = result.info;
    auto fail = [&](const std::string& why) { result.error = why; return result; };

    // 1) Cabecera
    NroHeader& h = info.header;
    if (!ReadAt(file, 0x10, h))       return fail("archivo demasiado pequeno para ser un NRO");
    if (h.magic != NRO_MAGIC)         return fail("no es un NRO (falta el magic \"NRO0\" en 0x10)");
    if (h.size > file.size())         return fail("el archivo esta cortado (size de la cabecera > tamano real)");
    if (base % PAGE != 0)             return fail("la direccion de carga debe estar alineada a 4 KB");

    // 2) Segmentos: deben ir en orden, alineados a pagina y dentro de la imagen
    if (h.text_offset != 0)                         return fail(".text debe empezar en el offset 0");
    if (h.ro_offset % PAGE || h.data_offset % PAGE) return fail("segmentos no alineados a 4 KB");
    if (u64(h.ro_offset) < u64(h.text_offset) + h.text_size ||
        u64(h.data_offset) < u64(h.ro_offset) + h.ro_size)    return fail("segmentos solapados o desordenados");
    if (u64(h.data_offset) + h.data_size > h.size)  return fail("los segmentos se salen de la imagen");

    // 3) Copiar la imagen a memoria. El .bss no esta en el archivo: es memoria a cero.
    memory.WriteBytes(base, file.data(), h.size);
    const u64 data_end = u64(h.data_offset) + h.data_size;
    const u64 bss_end  = AlignUp(data_end + h.bss_size, PAGE);
    const std::vector<u8> zeros(bss_end - data_end, 0);
    memory.WriteBytes(base + data_end, zeros.data(), zeros.size());

    // 4) Marcar zonas con sus permisos (como haria el kernel)
    memory.MapRegion(base + h.text_offset, AlignUp(h.text_size, PAGE), MemoryState::Code,
                     MemoryPermission::ReadExecute, ".text");
    memory.MapRegion(base + h.ro_offset, AlignUp(h.ro_size, PAGE), MemoryState::Code,
                     MemoryPermission::Read, ".rodata");
    memory.MapRegion(base + h.data_offset, bss_end - h.data_offset, MemoryState::CodeData,
                     MemoryPermission::ReadWrite, ".data + .bss");

    info.base = base;
    info.entry = base + h.text_offset;
    info.image_size = bss_end;

    // 5) Assets opcionales (nombre del juego/app)
    if (file.size() > h.size) ReadAssets(file, h.size, info);

    result.ok = true;
    return result;
}

} // namespace NeXo2::Loader
