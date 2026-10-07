#pragma once
// Texturas de la GPU Maxwell: descriptores TIC (imagen) y TSC (sampler) y muestreo.
//
// Un shader no recibe la textura directamente: recibe un "handle" de 32 bits
// (bits 0..19 = indice de imagen, 20..31 = indice de sampler). Con el indice se lee
// el descriptor de 32 bytes de la tabla de imagenes (SetTexHeaderPool) y el de la
// tabla de samplers (SetTexSamplerPool). El descriptor de imagen dice donde esta,
// su tamano, formato, mipmaps y como se colocan los texeles (block linear o lineal).
//
// Campos: cabeceras publicas de NVIDIA (open-gpu-doc, clb197tex.h: TEXHEAD_BL,
// TEXHEAD_PITCH y TEXSAMP). Ver docs/07-nexo-internals/gpu-textures.md.
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "gpu.hpp"
#include "shader.hpp"

namespace NeXo2::GPU {

// Descriptor de imagen (TIC) ya leido
struct TextureInfo {
    bool valid = false;
    u64 address = 0;
    u32 format = 0;              // COMPONENTS (tamano de componentes / compresion)
    u32 types[4] = {};           // tipo de dato de R, G, B, A (1 snorm .. 7 float)
    u32 swizzle[4] = {};         // fuente de X, Y, Z, W (0 cero, 2 R, 3 G, 4 B, 5 A, 6 uno entero, 7 uno float)
    bool srgb = false;
    u32 type = 1;                // 0 1D, 1 2D, 2 3D, 3 cubo, 4 1D array, 5 2D array, 6 buffer, 7 2D sin mip, 8 cubo array
    bool block_linear = true;
    u32 block_height_log2 = 0;   // GOBs por bloque (alto), en log2
    u32 block_depth_log2 = 0;
    u32 pitch = 0;               // solo lineal
    u32 width = 1, height = 1, depth = 1;   // depth = capas en los arrays
    u32 levels = 1;              // niveles de mipmap
    u32 min_level = 0, max_level = 0;       // vista (niveles que se pueden usar)
    bool normalized = true;      // coordenadas 0..1 (si no, en texeles)
    float lod_bias = 0;
};

// Descriptor de sampler (TSC) ya leido
struct SamplerInfo {
    u32 wrap[3] = {};            // 0 repetir, 1 espejo, 2 borde (clamp to edge), 3 color de borde, 4 clamp OpenGL, 5..7 espejo una vez
    bool depth_compare = false;
    u32 compare_func = 0;        // 0 nunca .. 7 siempre
    bool srgb = false;
    u32 mag_filter = 1;          // 1 punto, 2 lineal
    u32 min_filter = 1;          // 1 punto, 2 lineal, 3 anisotropico
    u32 mip_filter = 1;          // 1 sin mipmaps, 2 punto, 3 lineal
    float lod_bias = 0, min_lod = 0, max_lod = 15;
    bool force_unnormalized = false;
    float border[4] = {};
};

TextureInfo DecodeTic(const u32 w[8]);
SamplerInfo DecodeTsc(const u32 w[8]);

// Formato: bytes por texel (o por bloque de 4x4 en los comprimidos). 0 = no soportado
u32 TextureFormatBytes(u32 format);
bool TextureFormatCompressed(u32 format);
// Offset de un nivel de mipmap dentro de una capa, y tamano de una capa (block linear)
u64 TextureLevelOffset(const TextureInfo& t, u32 level);
u64 TextureLayerSize(const TextureInfo& t);

// Muestrea texturas para un draw. Guarda cada nivel ya decodificado (como valores de 32
// bits por componente) para no leer y decodificar la memoria en cada pixel.
class TextureSampler {
public:
    TextureSampler(Gpu& gpu, u64 tic_pool, u32 tic_max, u64 tsc_pool, u32 tsc_max)
        : m_gpu(gpu), m_ticPool(tic_pool), m_ticMax(tic_max), m_tscPool(tsc_pool), m_tscMax(tsc_max) {}
    // 'out' recibe 4 valores de 32 bits (floats, o enteros en formatos enteros)
    void Sample(u32 handle, const TextureRequest& req, u32 out[4]);
    // textureSize: ancho, alto, profundidad/capas y numero de niveles del nivel 'lod'
    void Query(u32 handle, u32 lod, u32 out[4]);
    // Otro muestreador de las mismas tablas, vacio (para otro hilo: cada uno con su cache)
    std::unique_ptr<TextureSampler> CloneEmpty() const {
        return std::make_unique<TextureSampler>(m_gpu, m_ticPool, m_ticMax, m_tscPool, m_tscMax);
    }

private:
    struct Level {
        u32 width = 0, height = 0, depth = 0;   // depth = capas o profundidad
        std::vector<std::array<u32, 4>> texels; // [z][y][x]
    };
    struct Texture {
        TextureInfo info;
        bool is_int = false;                    // formato entero: sin filtrar, sin convertir
        std::map<u32, Level> levels;
    };
    Texture* GetTexture(u32 tic);
    const Level& GetLevel(Texture& t, u32 level);
    SamplerInfo GetSampler(u32 tsc);
    void Texel(const Level& lv, s32 x, s32 y, s32 z, u32 out[4]) const;

    Gpu& m_gpu;
    u64 m_ticPool;
    u32 m_ticMax;
    u64 m_tscPool;
    u32 m_tscMax;
    std::map<u32, Texture> m_textures;
    std::map<u32, SamplerInfo> m_samplers;
};

} // namespace NeXo2::GPU
