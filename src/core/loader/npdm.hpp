#pragma once
#include <string>
#include <vector>
#include "common/types.hpp"

// main.npdm: los "metadatos" del programa de un juego (va en su ExeFS junto a los NSO).
// Dice como crear el proceso: prioridad, nucleo y pila del hilo principal, si es de 64 bits
// y su id de programa (title id). Lo demas (permisos de servicios y de SVC) NeXo no lo usa.
//
// Cabecera "META" (0x80 bytes):
//   0x0C flags (bit 0: 64 bits; bits 1-3: tipo de espacio de direcciones)
//   0x0E prioridad del hilo principal   0x0F nucleo del hilo principal
//   0x1C tamano de la pila principal    0x20 nombre (0x10 bytes)
//   0x70 offset/tamano de ACI0, offset/tamano de ACID
// "ACI0": 0x10 id del programa (u64)
namespace NeXo2::Loader {

struct NpdmInfo {
    bool is_64bit = true;
    u8   address_space = 3;          // 3 = 39 bits (lo normal en juegos de 64 bits)
    u8   main_priority = 44;
    u8   main_core = 0;
    u32  main_stack_size = 0x100000;
    u64  program_id = 0;
    std::string name;
};

struct NpdmResult {
    bool ok = false;
    std::string error;
    NpdmInfo info;
};

NpdmResult ParseNpdm(const std::vector<u8>& file);

} // namespace NeXo2::Loader
