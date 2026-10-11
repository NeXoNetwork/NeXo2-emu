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
#include <memory>
#include <string>
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
    // Indices "en linea" (VB_ELEMENT_U32 / U16 del pushbuffer, sin buffer de indices en
    // memoria): si no es nullptr, se usan estos en vez de leer el buffer de indices
    const std::vector<u32>* inline_indices = nullptr;
};

class TextureSampler;

class SoftwareRasterizer {
public:
    explicit SoftwareRasterizer(Gpu& gpu);
    ~SoftwareRasterizer();

    // Dibuja con el estado actual del motor 3D. 'regs' son sus registros.
    void Draw(const u32* regs, const ConstbufTable& cbs, const DrawCall& dc);

    // Un vertice ya procesado: las salidas del shader de vertices, a[0x000..0x3FF]
    struct Vertex {
        std::array<u32, 256> attr{};
        float Pos(u32 i) const;   // posicion de recorte (clip space): x, y, z, w en a[0x70..0x7C]
    };

    // Primera mitad de un draw, comun al dibujo por software y al de Vulkan: lee los
    // indices, ejecuta el shader de vertices en cada vertice (en la CPU) y junta los
    // triangulos (listas, tiras, abanicos, quads) en una lista.
    struct VertexStage {
        std::vector<Vertex> verts;
        // Triangulos: tres indices de 'verts' y el del vertice "que manda" (flat)
        std::vector<std::array<u32, 4>> tris;
        bool failed = false;     // el shader de vertices fallo (error)
        std::string error;
    };
    // 'tex' y 'tex_cb': texturas para el shader de vertices. false = no hay nada que dibujar.
    static bool RunVertexStage(Gpu& gpu, const u32* regs, const ConstbufTable& cbs, const DrawCall& dc,
                               TextureSampler* tex, u32 tex_cb, VertexStage& out);

private:
    struct ScreenVertex {
        const Vertex* v;
        float x, y, z;      // coordenadas de ventana (pixeles) y profundidad
        float inv_w;        // 1 / w (para interpolar con perspectiva)
    };
    // Un triangulo ya en pantalla, listo para recorrer sus pixeles
    struct RasterTri {
        ScreenVertex sv[3];
        const Vertex* provoking;
        bool front;
    };
    // Estado comun de un draw (se calcula una vez)
    struct DrawState;
    // Lo que cada hilo necesita para sus pixeles (interprete, programa, texturas...)
    struct Worker;
    // Hilos que se reparten las filas de la pantalla
    class ThreadPool;

    void ProcessTriangle(DrawState& st, const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& provoking);
    // Recorre los pixeles de todos los triangulos (st.tris), repartidos entre los hilos
    void RasterizeAll(DrawState& st);
    // Pixeles de un triangulo en las filas de este hilo: bandas de 4 filas, una de cada 'bands'
    void RasterizeTriangle(DrawState& st, Worker& w, const RasterTri& t, u32 band, u32 bands);

    Gpu& m_gpu;
    std::unique_ptr<ThreadPool> m_pool;
};

} // namespace NeXo2::GPU
