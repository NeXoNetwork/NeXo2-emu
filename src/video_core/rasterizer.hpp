#pragma once
// Dibujo por software: hace en la CPU lo que haria la GPU con un "draw".
//
//   1. Lee los vertices (vertex buffers + formato de cada atributo) y ejecuta el
//      shader de vertices en cada uno (shader.hpp).
//   2. Junta los vertices en triangulos (lista, tira, abanico, quads...).
//   3. Recorta lo que queda detras de la camara (plano cercano) y muy fuera de pantalla.
//   4. Pasa a coordenadas de pantalla (viewport) y descarta caras traseras (culling).
//   5. Recorre los pixeles de cada triangulo: interpola los atributos, ejecuta el
//      shader de pixeles, prueba de profundidad, mezcla (blending) y escribe el color.
//
// Todo con los registros del motor 3D (Maxwell3D); los numeros de registro son los de
// la documentacion publica de NVIDIA (open-gpu-doc, clb197.h) dividida entre 4.
// Ver docs/07-nexo-internals/gpu-rasterizer.md.
#include <array>
#include <vector>
#include "gpu.hpp"
#include "shader.hpp"
#include "surface.hpp"

namespace NeXo2::GPU {

// Constbuf enlazado a una etapa (c[0]..c[17])
struct ConstbufBinding {
    u64 address = 0;
    u32 size = 0;
    bool valid = false;
};
// [etapa][indice]: etapas 0 vertices, 1 teselado ctrl, 2 teselado eval, 3 geometria, 4 pixeles
using ConstbufTable = std::array<std::array<ConstbufBinding, 18>, 5>;

// Lo que pide un draw
struct DrawCall {
    u32 topology = 4;        // 0 puntos, 1 lineas, ... 4 triangulos (ver Maxwell3D VertexBeginGl)
    bool indexed = false;
    u32 first = 0;           // primer vertice o primer indice
    u32 count = 0;
    s32 vertex_offset = 0;   // indexado: se suma a cada indice
    u32 vertex_id_base = 0;  // no indexado: se suma a gl_VertexID
    u32 instance = 0;        // gl_InstanceID
    u32 base_instance = 0;
};

class SoftwareRasterizer {
public:
    explicit SoftwareRasterizer(Gpu& gpu) : m_gpu(gpu) {}

    // Dibuja con el estado actual del motor 3D. 'regs' son sus registros.
    void Draw(const u32* regs, const ConstbufTable& cbs, const DrawCall& dc);

    // Un vertice ya procesado: las salidas del shader de vertices, a[0x000..0x3FF]
    struct Vertex {
        std::array<u32, 256> attr{};
        float Pos(u32 i) const;   // posicion de recorte (clip space): x, y, z, w en a[0x70..0x7C]
    };

private:
    struct ScreenVertex {
        const Vertex* v;
        float x, y, z;      // coordenadas de ventana (pixeles) y profundidad
        float inv_w;        // 1 / w (para interpolar con perspectiva)
    };
    // Estado comun de un draw (se calcula una vez)
    struct DrawState;

    bool RunVertexShader(DrawState& st, u32 index, u32 vertex_id, Vertex& out);
    void ProcessTriangle(DrawState& st, const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& provoking);
    void RasterizeTriangle(DrawState& st, const ScreenVertex sv[3], const Vertex& provoking, bool front);

    Gpu& m_gpu;
};

} // namespace NeXo2::GPU
