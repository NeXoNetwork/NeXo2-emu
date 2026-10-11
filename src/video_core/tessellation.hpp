#pragma once
// Teselado (en la CPU, despues del shader de vertices):
//
//   1. Los vertices se juntan en parches de N (TessellationPatchSize, 0x373; primitiva 14).
//   2. Shader de control (TCS, etapa 2): un hilo por vertice de salida del parche (la SPH dice
//      cuantos). Lee los vertices del parche, escribe sus puntos de control y los atributos
//      del parche: niveles de teselado exteriores en a[0x00..0x0C] e interiores en a[0x10..0x14].
//      Sin TCS, los niveles salen de los registros 0x0C9..0x0CE.
//   3. Teselador (fijo): divide el dominio (triangulo, cuadrado o lineas) segun los niveles y
//      el modo (0x0C8: tipo, espaciado, sentido de giro).
//   4. Shader de evaluacion (TES, etapa 3): un hilo por punto del dominio. Lee las
//      coordenadas (u, v) en a[0x2F0], a[0x2F4], los puntos de control y los atributos del
//      parche, y escribe la posicion y las salidas como un shader de vertices.
// El resultado son triangulos (o lineas) normales: el resto del dibujo no cambia, y vale
// igual para el dibujo por software y para Vulkan.
// Ver docs/07-nexo-internals/gpu-compute-tessellation.md.
#include <array>
#include <string>
#include <vector>
#include "rasterizer.hpp"

namespace NeXo2::GPU {

class TextureSampler;

// Lo que sale del teselador: puntos (u, v) del dominio y como se unen
struct TessDomain {
    std::vector<std::array<float, 2>> points;
    std::vector<std::array<u32, 3>> tris;    // con el sentido de giro pedido
    std::vector<std::array<u32, 2>> lines;   // dominio de lineas (isolines)
};

// prim: 0 lineas, 1 triangulo, 2 cuadrado. spacing: 0 igual, 1 fraccional impar, 2 par.
// Devuelve vacio si el parche se descarta (algun nivel exterior <= 0).
TessDomain Tessellate(u32 prim, u32 spacing, bool cw, const float outer[4], const float inner[2]);

// Parches -> triangulos/lineas. 'slots' = vertices del draw (posicion en out.verts; -1 =
// reinicio). Los triangulos van a out.tris; las lineas (dominio de lineas) a 'lines'.
// false si un shader falla (motivo en out.error).
bool RunTessellation(Gpu& gpu, const u32* regs, const ConstbufTable& cbs, TextureSampler* tex, u32 tex_cb,
                     const std::vector<s64>& slots, SoftwareRasterizer::VertexStage& out,
                     std::vector<std::array<u32, 2>>& lines);

} // namespace NeXo2::GPU
