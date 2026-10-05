#pragma once
#include <string>
#include <vector>
#include "common/types.hpp"
#include "memory.hpp"

// Cargador de NRO: el formato ejecutable de los homebrew de Switch.
// Estructura del archivo: docs/04-formats/nro.md
//
//   0x00  RocrtHeader (0x10): normalmente "b <inicio>" + offset de MOD0
//   0x10  NroHeader   (0x70): magic "NRO0", tamano y segmentos
//   ...   .text  (codigo, R-X)
//   ...   .rodata (constantes, R--)
//   ...   .data  (variables, RW-)   + .bss (variables a cero, no esta en el archivo)
//   [opcional, tras 'size'] AssetHeader "ASET": icono, NACP (nombre/autor), RomFS
//
// Un NRO es una imagen "plana": cada segmento esta en el archivo en el mismo
// offset que tendra en memoria, asi que cargarlo es copiar y marcar zonas.
namespace NeXo2::Loader {

struct NroHeader {
    u32 magic;              // "NRO0"
    u32 version;
    u32 size;               // tamano de la imagen (sin assets)
    u32 flags;
    u32 text_offset, text_size;
    u32 ro_offset,   ro_size;
    u32 data_offset, data_size;
    u32 bss_size;
    u32 reserved;
    u8  module_id[0x20];    // build id
    u32 dso_handle_offset;
    u32 reserved2;
    u32 embedded_offset, embedded_size;
    u32 dynstr_offset,   dynstr_size;
    u32 dynsym_offset,   dynsym_size;
};
static_assert(sizeof(NroHeader) == 0x70, "NroHeader debe medir 0x70 bytes");

struct NroInfo {
    u64 base = 0;           // direccion de carga
    u64 entry = 0;          // primera instruccion (= base: el "b" del RocrtHeader)
    u64 image_size = 0;     // texto + rodata + data + bss, alineado a pagina
    NroHeader header{};
    bool has_assets = false;
    std::string title;      // del NACP, si hay assets
    std::string author;
};

struct NroLoadResult {
    bool ok = false;
    std::string error;      // explicacion si ok == false
    NroInfo info;
};

// Comprueba el NRO, lo copia a 'base' y marca sus zonas en el mapa de memoria.
NroLoadResult LoadNro(const std::vector<u8>& file, Core::Memory& memory, u64 base);

} // namespace NeXo2::Loader
