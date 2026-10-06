#pragma once
// Superficies (imagenes) de la GPU Maxwell: como se colocan los pixeles en memoria
// ("pitch" = lineal, o "block linear" = en bloques) y como se escribe un color en
// cada formato. Lo usan el 3D (borrar render targets), la copia (DMA) y el 2D.
// Ver docs/07-nexo-internals/gpu.md.
#include <array>
#include "common/types.hpp"

namespace NeXo2::GPU {

// Direccion (en bytes, desde el inicio de la imagen) del byte (x_bytes, y) de una
// imagen block linear: GOBs de 64 bytes x 8 filas, apilados en bloques de 2^block_h GOBs.
//   width_bytes: ancho de la imagen en bytes (se redondea a GOBs de 64)
u64 BlockLinearOffset(u32 x_bytes, u32 y, u32 width_bytes, u32 block_height_log2);

// Descripcion de una superficie en memoria de la GPU
struct Surface {
    u64 address = 0;          // direccion virtual de la GPU
    u32 width = 0;            // en pixeles
    u32 height = 0;
    u32 bytes_per_pixel = 4;
    bool linear = false;      // true = pitch (filas seguidas), false = block linear
    u32 pitch = 0;            // bytes por fila (solo lineal)
    u32 block_height_log2 = 0;

    // Desplazamiento en bytes del pixel (x, y)
    u64 Offset(u32 x, u32 y) const {
        return linear ? u64(y) * pitch + u64(x) * bytes_per_pixel
                      : BlockLinearOffset(x * bytes_per_pixel, y, width * bytes_per_pixel, block_height_log2);
    }
};

// --- Formatos de render target (color) ---
// Bytes por pixel de un formato de color (ColorSurfaceFormat); 0 = desconocido
u32 ColorFormatBytes(u32 format);
// Codifica un color (4 floats RGBA) en el formato; 'out' recibe ColorFormatBytes() bytes.
// Devuelve false si el formato no se sabe escribir.
bool EncodeColor(u32 format, const float rgba[4], u8* out);
// Mascara de bytes por componente (para borrar solo R, G, B o A): bit i = byte i se escribe.
// Si el formato no tiene componentes separables por byte, devuelve todos.
u32 ColorWriteByteMask(u32 format, u32 component_mask);

// --- Formatos de profundidad / stencil ---
u32  DepthFormatBytes(u32 format);
// Escribe profundidad y/o stencil (segun write_depth/write_stencil) en el pixel 'px'
bool EncodeDepthStencil(u32 format, float depth, u8 stencil, bool write_depth, bool write_stencil, u8* px);

} // namespace NeXo2::GPU
