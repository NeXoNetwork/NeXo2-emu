#pragma once
#include <string>
#include <vector>
#include "common/types.hpp"
#include "memory.hpp"

// Cargador de NSO: el formato de los ejecutables de los juegos de Switch (1 y 2).
// Un juego trae varios en su ExeFS: rtld (el enlazador), main (el juego), subsdk0..9
// (librerias) y sdk (el SDK de Nintendo). Se cargan seguidos en memoria y se empieza por
// el primero (rtld), que enlaza los demas entre si.
//
// Estructura (cabecera de 0x100 bytes):
//   0x00 "NSO0", version, reservado, flags (bits 0-2: .text/.rodata/.data comprimidos con LZ4)
//   0x10 .text   {offset en el archivo, offset en memoria, tamano descomprimido}
//   0x20 .rodata {igual}
//   0x30 .data   {igual}, 0x3C tamano del .bss
//   0x40 id del modulo (build id, 0x20 bytes)
//   0x60 tamanos de .text/.rodata/.data en el archivo (comprimidos)
// A diferencia del NRO, cada segmento puede ir comprimido y en un sitio distinto del archivo.
namespace NeXo2::Loader {

struct NsoSegment { u32 file_offset, memory_offset, size; };

struct NsoHeader {
    u32 magic;              // "NSO0"
    u32 version;
    u32 reserved;
    u32 flags;
    NsoSegment text;
    u32 module_name_offset;
    NsoSegment ro;
    u32 module_name_size;
    NsoSegment data;
    u32 bss_size;
    u8  module_id[0x20];
    u32 text_file_size, ro_file_size, data_file_size;
    u8  reserved2[0x1C];
    u32 api_info_offset, api_info_size;
    u32 dynstr_offset, dynstr_size;
    u32 dynsym_offset, dynsym_size;
    u8  hashes[3][0x20];
};
static_assert(sizeof(NsoHeader) == 0x100, "NsoHeader debe medir 0x100 bytes");

struct NsoLoadResult {
    bool ok = false;
    std::string error;
    u64 image_size = 0;     // lo que ocupa en memoria (con el .bss), alineado a 4 KB
};

// Descomprime un bloque LZ4 (sin cabecera de "frame"). false si los datos no cuadran.
bool Lz4DecompressBlock(const u8* src, size_t src_size, u8* dst, size_t dst_size);

// Comprueba el NSO, lo descomprime en 'base' y marca sus zonas ('name' para el mapa).
NsoLoadResult LoadNso(const std::vector<u8>& file, Core::Memory& memory, u64 base, const std::string& name);

} // namespace NeXo2::Loader
