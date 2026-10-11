#pragma once
// Compute: ejecuta un "dispatch" del motor de compute (clase 0xB1C0) en la CPU.
//
// El programa no manda los parametros con registros sino con una QMD ("queue meta data",
// 0x100 bytes en memoria de la GPU): SendPcasA = direccion de la QMD >> 8, y
// SendSignalingPcasB la lanza. La QMD dice:
//   - donde esta el programa (offset dentro de SetProgramRegion; compute no tiene SPH),
//   - cuantos grupos (cta_raster: x, y, z) y cuantos hilos por grupo (cta_thread_dimension),
//   - cuanta memoria compartida usa cada grupo,
//   - los constbufs c[0]..c[7] (direccion, tamano y mascara de validos).
//
// Cada grupo se ejecuta entero antes del siguiente. Dentro del grupo, cada hilo tiene su
// propio interprete (registros, pila, memoria local); un BAR.SYNC para al hilo y se sigue
// con el siguiente; cuando todos han llegado, se continua (ShaderInterpreter::Resume).
// Ver docs/07-nexo-internals/gpu-compute-tessellation.md.
#include <string>
#include "gpu.hpp"

namespace NeXo2::GPU {

struct ComputeQmd {
    u64 program = 0;              // direccion del codigo
    u32 grid[3] = {1, 1, 1};      // grupos
    u32 block[3] = {1, 1, 1};     // hilos por grupo
    u32 shared_size = 0;          // bytes de memoria compartida por grupo
    struct Cb { u64 address = 0; u32 size = 0; bool valid = false; } cb[8];
};

// Lee la QMD de 'qmd_addr'. 'program_region' = SetProgramRegion del motor de compute.
ComputeQmd ReadComputeQmd(Gpu& gpu, u64 qmd_addr, u64 program_region);

// Ejecuta el dispatch con los registros del motor de compute (texturas: 0x557/0x55D/0x982).
// false si fallo (instruccion desconocida...); el motivo queda en 'error'.
bool RunCompute(Gpu& gpu, const u32* regs, const ComputeQmd& qmd, std::string& error);

} // namespace NeXo2::GPU
