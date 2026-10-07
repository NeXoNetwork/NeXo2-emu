// Texturas: descriptores, formatos, colocacion en memoria y muestreo.
// Ver texture.hpp y docs/07-nexo-internals/gpu-textures.md.
#include "texture.hpp"
#include "surface.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace NeXo2::GPU {

namespace {

inline float F(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
inline u32 U(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }
inline u32 Bits(const u32* w, u32 pos, u32 len) {   // campo de un descriptor de 256 bits
    const u64 v = (u64(w[pos / 32 + 1 < 8 ? pos / 32 + 1 : 7]) << 32) | w[pos / 32];
    return u32((v >> (pos % 32)) & ((1ull << len) - 1));
}
// Punto fijo con signo de 13 bits (4.8) -> float
inline float Fixed13(u32 v) { return float(s32(v << 19) >> 19) / 256.0f; }

float HalfToFloat(u32 h) {
    const u32 sign = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f;
    if (e == 0) f = std::ldexp(float(m), -24);
    else if (e == 31) f = m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else f = std::ldexp(float(m | 0x400), int(e) - 25);
    return sign ? -f : f;
}
// Floats pequenos sin signo (formato R11G11B10F): 'mbits' bits de mantisa y 5 de exponente
float SmallFloat(u32 v, u32 mbits) {
    const u32 e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return std::ldexp(float(m), -14 - int(mbits));
    if (e == 31) return m ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    return std::ldexp(float(m | (1u << mbits)), int(e) - 15 - int(mbits));
}
float SrgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

// Componentes de cada formato sin comprimir: posicion y tamano en bits de R, G, B, A
struct Layout { u32 bytes; u8 pos[4]; u8 size[4]; };
bool GetLayout(u32 f, Layout& l) {
    switch (f) {
        case 0x01: l = {16, {0, 32, 64, 96}, {32, 32, 32, 32}}; return true;   // R32_G32_B32_A32
        case 0x02: l = {12, {0, 32, 64, 0}, {32, 32, 32, 0}}; return true;     // R32_G32_B32
        case 0x03: l = {8, {0, 16, 32, 48}, {16, 16, 16, 16}}; return true;    // R16_G16_B16_A16
        case 0x04: l = {8, {0, 32, 0, 0}, {32, 32, 0, 0}}; return true;        // R32_G32
        case 0x07: l = {4, {0, 8, 16, 0}, {8, 8, 8, 0}}; return true;          // X8B8G8R8
        case 0x08: l = {4, {0, 8, 16, 24}, {8, 8, 8, 8}}; return true;         // A8B8G8R8
        case 0x09: l = {4, {0, 10, 20, 30}, {10, 10, 10, 2}}; return true;     // A2B10G10R10
        case 0x0C: l = {4, {0, 16, 0, 0}, {16, 16, 0, 0}}; return true;        // R16_G16
        case 0x0F: l = {4, {0, 0, 0, 0}, {32, 0, 0, 0}}; return true;          // R32
        case 0x12: l = {2, {0, 4, 8, 12}, {4, 4, 4, 4}}; return true;          // A4B4G4R4
        case 0x13: l = {2, {0, 1, 6, 11}, {1, 5, 5, 5}}; return true;          // A5B5G5R1
        case 0x14: l = {2, {0, 5, 10, 15}, {5, 5, 5, 1}}; return true;         // A1B5G5R5
        case 0x15: l = {2, {0, 5, 11, 0}, {5, 6, 5, 0}}; return true;          // B5G6R5
        case 0x16: l = {2, {0, 5, 10, 0}, {5, 5, 6, 0}}; return true;          // B6G5R5
        case 0x18: l = {2, {0, 8, 0, 0}, {8, 8, 0, 0}}; return true;           // G8R8
        case 0x1B: l = {2, {0, 0, 0, 0}, {16, 0, 0, 0}}; return true;          // R16
        case 0x1D: l = {1, {0, 0, 0, 0}, {8, 0, 0, 0}}; return true;           // R8
        case 0x1E: l = {1, {0, 4, 0, 0}, {4, 4, 0, 0}}; return true;           // G4R4
        // Profundidad: R = Z, G = stencil
        case 0x29: l = {4, {8, 0, 0, 0}, {24, 8, 0, 0}}; return true;          // Z24S8
        case 0x2A: l = {4, {0, 0, 0, 0}, {24, 0, 0, 0}}; return true;          // X8Z24
        case 0x2B: l = {4, {0, 24, 0, 0}, {24, 8, 0, 0}}; return true;         // S8Z24
        case 0x2F: l = {4, {0, 0, 0, 0}, {32, 0, 0, 0}}; return true;          // ZF32
        case 0x30: l = {8, {0, 32, 0, 0}, {32, 8, 0, 0}}; return true;         // ZF32_X24S8
        case 0x3A: l = {2, {0, 0, 0, 0}, {16, 0, 0, 0}}; return true;          // Z16
        default: return false;
    }
}

u64 ReadBitsLE(const u8* p, u32 pos, u32 size) {   // 'size' bits desde el bit 'pos' (hasta 32)
    u64 v = 0;
    const u32 first = pos / 8, last = (pos + size + 7) / 8;
    for (u32 i = first; i < last; ++i) v |= u64(p[i]) << (8 * (i - first));
    return (v >> (pos % 8)) & ((1ull << size) - 1);
}

// Convierte un componente crudo segun su tipo de dato. Devuelve los 32 bits que vera el shader.
u32 ConvertComponent(u32 raw, u32 bits, u32 type, bool srgb) {
    switch (type) {
        case 1: case 5: {   // snorm
            const s32 sv = bits < 32 ? s32(raw << (32 - bits)) >> (32 - bits) : s32(raw);
            return U(std::max(float(sv) / float((1u << (bits - 1)) - 1), -1.0f));
        }
        case 3: return bits < 32 ? u32(s32(raw << (32 - bits)) >> (32 - bits)) : raw;   // sint
        case 4: return raw;                                                              // uint
        case 7:                                                                          // float
            if (bits == 32) return raw;
            if (bits == 16) return U(HalfToFloat(raw));
            return U(float(raw));
        default: {          // unorm (2, 6 y los que no se conocen)
            float v = bits >= 32 ? float(double(raw) / 4294967295.0) : float(raw) / float((1u << bits) - 1);
            if (srgb) v = SrgbToLinear(v);
            return U(v);
        }
    }
}

// --- Formatos comprimidos (bloques de 4x4 texeles) ---
void Rgb565(u16 c, float out[3]) {
    out[0] = float(c >> 11) / 31.0f;          // en BC1 el rojo va en los bits altos
    out[1] = float((c >> 5) & 63) / 63.0f;
    out[2] = float(c & 31) / 31.0f;
}
// Colores de un bloque BC1; 'four' = siempre 4 colores (BC2/BC3)
void DecodeBc1Colors(const u8* b, bool four, float rgba[16][4]) {
    const u16 c0 = u16(b[0] | (b[1] << 8)), c1 = u16(b[2] | (b[3] << 8));
    float p[4][4];
    Rgb565(c0, p[0]); Rgb565(c1, p[1]);
    p[0][3] = p[1][3] = 1.0f;
    if (four || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            p[2][i] = (2 * p[0][i] + p[1][i]) / 3.0f;
            p[3][i] = (p[0][i] + 2 * p[1][i]) / 3.0f;
        }
        p[2][3] = p[3][3] = 1.0f;
    } else {
        for (int i = 0; i < 3; ++i) { p[2][i] = (p[0][i] + p[1][i]) / 2.0f; p[3][i] = 0.0f; }
        p[2][3] = 1.0f; p[3][3] = 0.0f;   // negro transparente
    }
    const u32 idx = u32(b[4]) | (u32(b[5]) << 8) | (u32(b[6]) << 16) | (u32(b[7]) << 24);
    for (int t = 0; t < 16; ++t) std::memcpy(rgba[t], p[(idx >> (2 * t)) & 3], sizeof(float) * 4);
}
// Un canal BC4 (8 bytes): dos extremos y 16 indices de 3 bits
void DecodeBc4(const u8* b, bool snorm, float out[16]) {
    float e0, e1;
    if (snorm) {
        e0 = std::max(float(s8(b[0])) / 127.0f, -1.0f);
        e1 = std::max(float(s8(b[1])) / 127.0f, -1.0f);
    } else {
        e0 = b[0] / 255.0f;
        e1 = b[1] / 255.0f;
    }
    float p[8] = {e0, e1};
    const bool six = snorm ? s8(b[0]) > s8(b[1]) : b[0] > b[1];
    if (six) {
        for (int i = 1; i < 7; ++i) p[i + 1] = ((7 - i) * e0 + i * e1) / 7.0f;
    } else {
        for (int i = 1; i < 5; ++i) p[i + 1] = ((5 - i) * e0 + i * e1) / 5.0f;
        p[6] = snorm ? -1.0f : 0.0f;
        p[7] = 1.0f;
    }
    u64 idx = 0;
    for (int i = 0; i < 6; ++i) idx |= u64(b[2 + i]) << (8 * i);
    for (int t = 0; t < 16; ++t) out[t] = p[(idx >> (3 * t)) & 7];
}

// Decodifica un bloque comprimido en 16 texeles RGBA (floats)
bool DecodeBlock(u32 format, const u8* b, bool snorm, bool srgb, float rgba[16][4]) {
    switch (format) {
        case 0x24: DecodeBc1Colors(b, false, rgba); break;                    // DXT1 / BC1
        case 0x25:                                                            // DXT23 / BC2
            DecodeBc1Colors(b + 8, true, rgba);
            for (int t = 0; t < 16; ++t) rgba[t][3] = float((b[t / 2] >> (4 * (t & 1))) & 15) / 15.0f;
            break;
        case 0x26: {                                                          // DXT45 / BC3
            float a[16];
            DecodeBc4(b, false, a);
            DecodeBc1Colors(b + 8, true, rgba);
            for (int t = 0; t < 16; ++t) rgba[t][3] = a[t];
            break;
        }
        case 0x27: {                                                          // DXN1 / BC4
            float r[16];
            DecodeBc4(b, snorm, r);
            for (int t = 0; t < 16; ++t) { rgba[t][0] = r[t]; rgba[t][1] = rgba[t][2] = 0; rgba[t][3] = 1; }
            return true;
        }
        case 0x28: {                                                          // DXN2 / BC5
            float r[16], g[16];
            DecodeBc4(b, snorm, r);
            DecodeBc4(b + 8, snorm, g);
            for (int t = 0; t < 16; ++t) { rgba[t][0] = r[t]; rgba[t][1] = g[t]; rgba[t][2] = 0; rgba[t][3] = 1; }
            return true;
        }
        default: return false;
    }
    if (srgb)
        for (int t = 0; t < 16; ++t)
            for (int c = 0; c < 3; ++c) rgba[t][c] = SrgbToLinear(rgba[t][c]);
    return true;
}

// Offset de un texel (o bloque) en una superficie block linear de 3 dimensiones:
// GOBs de 64 bytes x 8 filas, agrupados en bloques de 1 x 2^bh x 2^bd GOBs
u64 BlockLinear3D(u32 xb, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh, u32 bd) {
    const u32 gobs_x = (width_bytes + 63) / 64;
    const u32 block_rows = 8u << bh, block_depth = 1u << bd;
    const u32 blocks_y = (height + block_rows - 1) / block_rows;
    const u64 block_size = 512ull << (bh + bd);
    const u64 block = (u64(z / block_depth) * blocks_y + y / block_rows) * gobs_x + xb / 64;
    const u32 gob = (z % block_depth) * (1u << bh) + (y % block_rows) / 8;
    const u32 gx = xb % 64, gy = y % 8;
    const u32 in_gob = (gx / 32) * 256 + (gy / 2) * 64 + ((gx % 32) / 16) * 32 + (gy % 2) * 16 + (gx % 16);
    return block * block_size + u64(gob) * 512 + in_gob;
}

// Ajuste del alto de bloque para un nivel pequeno (como hace el hardware con los mipmaps)
u32 AdjustShift(u32 shift, u32 unit, u32 dim) {
    if (!shift) return 0;
    u32 x = unit << (shift - 1);
    if (x >= dim) {
        while (--shift) {
            x >>= 1;
            if (x < dim) break;
        }
    }
    return shift;
}

bool IsIntType(u32 t) { return t == 3 || t == 4; }

s32 WrapInt(s32 i, u32 mode, s32 size, bool& border) {
    border = false;
    switch (mode) {
        case 0: { s32 m = i % size; return m < 0 ? m + size : m; }
        case 1: {
            s32 period = 2 * size, m = i % period;
            if (m < 0) m += period;
            return m < size ? m : period - 1 - m;
        }
        case 3: case 6:                                              // color de borde (6: tras el espejo)
            if (mode == 6 && i < 0) i = -i - 1;
            if (i < 0 || i >= size) border = true;
            return std::clamp(i, 0, size - 1);
        case 5: case 7: return std::clamp(i < 0 ? -i - 1 : i, 0, size - 1);   // espejo una vez
        default: return std::clamp(i, 0, size - 1);
    }
}

bool Compare(u32 func, float ref, float v) {
    switch (func & 7) {
        case 0: return false;
        case 1: return ref < v;
        case 2: return ref == v;
        case 3: return ref <= v;
        case 4: return ref > v;
        case 5: return ref != v;
        case 6: return ref >= v;
        default: return true;
    }
}

} // namespace

// ============================================================================
//  Descriptores
// ============================================================================

TextureInfo DecodeTic(const u32 w[8]) {
    TextureInfo t;
    t.format = Bits(w, 0, 7);
    for (u32 i = 0; i < 4; ++i) {
        t.types[i] = Bits(w, 7 + 3 * i, 3);
        t.swizzle[i] = Bits(w, 19 + 3 * i, 3);
    }
    const u32 version = Bits(w, 85, 3);
    t.block_linear = version == 3 || version == 4;
    const u64 hi = u64(Bits(w, 64, 16)) << 32;
    if (t.block_linear) {
        t.address = hi | (w[1] & 0xFFFFFE00u);
        t.block_height_log2 = Bits(w, 99, 3);
        t.block_depth_log2 = Bits(w, 102, 3);
    } else {
        t.address = hi | (w[1] & 0xFFFFFFE0u);
        t.pitch = Bits(w, 96, 16) << 5;
    }
    t.levels = Bits(w, 124, 4) + 1;
    t.width = Bits(w, 128, 16) + 1;
    t.srgb = Bits(w, 150, 1);
    t.type = Bits(w, 151, 4);
    t.height = Bits(w, 160, 16) + 1;
    t.depth = Bits(w, 176, 14) + 1;
    t.normalized = Bits(w, 191, 1);
    t.lod_bias = Fixed13(Bits(w, 198, 13));
    t.min_level = Bits(w, 224, 4);
    t.max_level = Bits(w, 228, 4);
    if (t.max_level < t.min_level || t.max_level >= t.levels) t.max_level = t.levels - 1;
    if (t.min_level >= t.levels) t.min_level = 0;
    t.valid = version != 0 && TextureFormatBytes(t.format) != 0;
    return t;
}

SamplerInfo DecodeTsc(const u32 w[8]) {
    SamplerInfo s;
    s.wrap[0] = w[0] & 7;
    s.wrap[1] = (w[0] >> 3) & 7;
    s.wrap[2] = (w[0] >> 6) & 7;
    s.depth_compare = (w[0] >> 9) & 1;
    s.compare_func = (w[0] >> 10) & 7;
    s.srgb = (w[0] >> 13) & 1;
    s.mag_filter = w[1] & 7;
    s.min_filter = (w[1] >> 4) & 3;
    s.mip_filter = (w[1] >> 6) & 3;
    s.lod_bias = Fixed13((w[1] >> 12) & 0x1FFF);
    s.force_unnormalized = (w[1] >> 25) & 1;
    s.min_lod = float(w[2] & 0xFFF) / 256.0f;
    s.max_lod = float((w[2] >> 12) & 0xFFF) / 256.0f;
    for (int i = 0; i < 4; ++i) s.border[i] = F(w[4 + i]);
    return s;
}

u32 TextureFormatBytes(u32 format) {
    Layout l;
    if (GetLayout(format, l)) return l.bytes;
    switch (format) {
        case 0x20: case 0x21: return 4;                    // E5B9G9R9, R11G11B10F
        case 0x24: case 0x27: return 8;                    // BC1, BC4
        case 0x25: case 0x26: case 0x28: return 16;        // BC2, BC3, BC5
        default: return 0;
    }
}

bool TextureFormatCompressed(u32 format) { return format >= 0x24 && format <= 0x28; }

u64 TextureLevelOffset(const TextureInfo& t, u32 level) {
    if (!t.block_linear) return 0;
    const u32 bpb = TextureFormatBytes(t.format);
    const u32 bw = TextureFormatCompressed(t.format) ? 4 : 1;
    const bool is3d = t.type == 2;
    u64 offset = 0;
    for (u32 i = 0; i < level; ++i) {
        const u32 w = std::max(t.width >> i, 1u), h = std::max(t.height >> i, 1u);
        const u32 d = is3d ? std::max(t.depth >> i, 1u) : 1;
        const u32 wb = ((w + bw - 1) / bw) * bpb, hb = (h + bw - 1) / bw;
        const u32 bh = AdjustShift(t.block_height_log2, 8, hb);
        const u32 bd = AdjustShift(t.block_depth_log2, 1, d);
        const u64 gobs_x = (wb + 63) / 64;
        const u64 tiles_y = ((hb + 7) / 8 + (1u << bh) - 1) >> bh;
        const u64 tiles_z = (d + (1u << bd) - 1) >> bd;
        offset += (gobs_x * tiles_y * tiles_z) << (9 + bh + bd);
    }
    return offset;
}

u64 TextureLayerSize(const TextureInfo& t) {
    if (!t.block_linear) return u64(t.pitch) * t.height;
    u64 size = TextureLevelOffset(t, t.levels);
    // Alinear al tamano de bloque del nivel 0 (como cada capa empieza en un bloque)
    const u32 bw = TextureFormatCompressed(t.format) ? 4 : 1;
    const u32 hb = (t.height + bw - 1) / bw;
    const u32 bh = AdjustShift(t.block_height_log2, 8, hb);
    const u32 bd = AdjustShift(t.block_depth_log2, 1, t.type == 2 ? t.depth : 1);
    const u32 shift = 9 + bh + bd;
    const u64 gobs = size >> shift;
    return (gobs << shift) == size ? size : (gobs + 1) << shift;
}

// ============================================================================
//  Muestreo
// ============================================================================

TextureSampler::Texture* TextureSampler::GetTexture(u32 tic) {
    auto it = m_textures.find(tic);
    if (it != m_textures.end()) return it->second.info.valid ? &it->second : nullptr;
    Texture& t = m_textures[tic];
    if (tic > m_ticMax || !m_ticPool) return nullptr;
    u32 w[8];
    m_gpu.MemoryManager().ReadBlock(m_ticPool + u64(tic) * 32, w, sizeof(w));
    t.info = DecodeTic(w);
    if (!t.info.valid) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "textura con formato 0x%X no soportado (o descriptor vacio)", Bits(w, 0, 7));
        m_gpu.Warn("texfmt" + std::to_string(Bits(w, 0, 7)), buf);
        return nullptr;
    }
    t.is_int = IsIntType(t.info.types[0]) && !TextureFormatCompressed(t.info.format) &&
               t.info.format != 0x20 && t.info.format != 0x21;
    return &t;
}

SamplerInfo TextureSampler::GetSampler(u32 tsc) {
    auto it = m_samplers.find(tsc);
    if (it != m_samplers.end()) return it->second;
    u32 w[8] = {};
    if (tsc <= m_tscMax && m_tscPool) m_gpu.MemoryManager().ReadBlock(m_tscPool + u64(tsc) * 32, w, sizeof(w));
    SamplerInfo s = DecodeTsc(w);
    m_samplers[tsc] = s;
    return s;
}

const TextureSampler::Level& TextureSampler::GetLevel(Texture& tex, u32 level) {
    auto it = tex.levels.find(level);
    if (it != tex.levels.end()) return it->second;
    Level& lv = tex.levels[level];
    const TextureInfo& t = tex.info;
    const bool is3d = t.type == 2;
    lv.width = std::max(t.width >> level, 1u);
    lv.height = std::max(t.height >> level, 1u);
    lv.depth = is3d ? std::max(t.depth >> level, 1u) : t.depth;   // capas en los arrays
    const u64 texels = u64(lv.width) * lv.height * lv.depth;
    if (texels > (64ull << 20)) {   // mas de 64 millones de texeles: algo esta mal
        m_gpu.Warn("texbig", "textura demasiado grande, se ignora");
        lv.width = lv.height = lv.depth = 1;
        lv.texels.assign(1, {0, 0, 0, 0});
        return lv;
    }
    lv.texels.assign(size_t(texels), {0, 0, 0, U(1.0f)});
    auto& mm = m_gpu.MemoryManager();
    const u32 bpb = TextureFormatBytes(t.format);
    const bool comp = TextureFormatCompressed(t.format);
    const u32 bw = comp ? 4 : 1;
    const u32 wblocks = (lv.width + bw - 1) / bw, hblocks = (lv.height + bw - 1) / bw;
    const u32 wb = wblocks * bpb;
    const u32 bh = AdjustShift(t.block_height_log2, 8, hblocks);
    const u32 bd = is3d ? AdjustShift(t.block_depth_log2, 1, lv.depth) : 0;
    const u64 level_off = TextureLevelOffset(t, level);
    const u64 layer_size = TextureLayerSize(t);
    Layout lay{};
    GetLayout(t.format, lay);
    const bool snorm = t.types[0] == 1 || t.types[0] == 5;

    // Leer cada capa entera de una vez y decodificar desde ahi
    std::vector<u8> raw;
    for (u32 z = 0; z < lv.depth; ++z) {
        u64 base;
        u32 zin = 0;
        if (!t.block_linear) {
            base = t.address + (is3d ? 0 : u64(z) * layer_size);
        } else if (is3d) {
            base = t.address + level_off;
            zin = z;
        } else {
            base = t.address + u64(z) * layer_size + level_off;
        }
        auto addr_of = [&](u32 bx, u32 by) -> u64 {
            if (!t.block_linear) return base + u64(by) * t.pitch + u64(bx) * bpb;
            return base + BlockLinear3D(bx * bpb, by, zin, wb, hblocks, bh, bd);
        };
        u8 block[16];
        for (u32 by = 0; by < hblocks; ++by) {
            for (u32 bx = 0; bx < wblocks; ++bx) {
                mm.ReadBlock(addr_of(bx, by), block, bpb);
                if (comp) {
                    float rgba[16][4];
                    if (!DecodeBlock(t.format, block, snorm, t.srgb, rgba)) continue;
                    for (u32 ty = 0; ty < 4; ++ty)
                        for (u32 tx = 0; tx < 4; ++tx) {
                            const u32 x = bx * 4 + tx, y = by * 4 + ty;
                            if (x >= lv.width || y >= lv.height) continue;
                            auto& o = lv.texels[(size_t(z) * lv.height + y) * lv.width + x];
                            for (int c = 0; c < 4; ++c) o[c] = U(rgba[ty * 4 + tx][c]);
                        }
                    continue;
                }
                auto& o = lv.texels[(size_t(z) * lv.height + by) * lv.width + bx];
                if (t.format == 0x21) {          // R11G11B10F
                    u32 v; std::memcpy(&v, block, 4);
                    o = {U(SmallFloat(v & 0x7FF, 6)), U(SmallFloat((v >> 11) & 0x7FF, 6)), U(SmallFloat(v >> 22, 5)), U(1.0f)};
                    continue;
                }
                if (t.format == 0x20) {          // E5B9G9R9 (exponente compartido)
                    u32 v; std::memcpy(&v, block, 4);
                    const float scale = std::ldexp(1.0f, int(v >> 27) - 15 - 9);
                    o = {U(float(v & 0x1FF) * scale), U(float((v >> 9) & 0x1FF) * scale), U(float((v >> 18) & 0x1FF) * scale), U(1.0f)};
                    continue;
                }
                for (u32 c = 0; c < 4; ++c) {
                    if (!lay.size[c]) {
                        o[c] = c == 3 ? (IsIntType(t.types[0]) ? 1u : U(1.0f)) : 0;
                        continue;
                    }
                    const u32 rawc = u32(ReadBitsLE(block, lay.pos[c], lay.size[c]));
                    o[c] = ConvertComponent(rawc, lay.size[c], t.types[c] ? t.types[c] : t.types[0], t.srgb && c < 3);
                }
            }
        }
    }
    return lv;
}

void TextureSampler::Texel(const Level& lv, s32 x, s32 y, s32 z, u32 out[4]) const {
    const auto& t = lv.texels[(size_t(z) * lv.height + u32(y)) * lv.width + u32(x)];
    for (int c = 0; c < 4; ++c) out[c] = t[c];
}

void TextureSampler::Sample(u32 handle, const TextureRequest& req, u32 out[4]) {
    out[0] = out[1] = out[2] = 0;
    out[3] = U(1.0f);
    Texture* tex = GetTexture(handle & 0xFFFFF);
    if (!tex) return;
    const TextureInfo& t = tex->info;
    const SamplerInfo s = GetSampler(handle >> 20);
    const bool is3d = t.type == 2;
    TextureRequest r = req;
    if ((t.type == 3 || t.type == 8) && !r.fetch) {
        // Cubo: el eje mas largo de la direccion elige la cara (capa 0..5), y los otros dos
        // dan la posicion dentro de ella (tabla de OpenGL)
        const float x = r.coords[0], y = r.coords[1], z = r.coords[2];
        const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
        u32 face;
        float sc, tc, ma;
        if (ax >= ay && ax >= az) { face = x >= 0 ? 0 : 1; sc = x >= 0 ? -z : z; tc = -y; ma = ax; }
        else if (ay >= az)        { face = y >= 0 ? 2 : 3; sc = x; tc = y >= 0 ? z : -z; ma = ay; }
        else                      { face = z >= 0 ? 4 : 5; sc = z >= 0 ? x : -x; tc = -y; ma = az; }
        if (ma == 0) ma = 1;
        r.coords[0] = (sc / ma + 1) * 0.5f;
        r.coords[1] = (tc / ma + 1) * 0.5f;
        r.coords[2] = 0;
        r.layer = r.layer * 6 + face;
    }
    u32 raw[4];

    if (r.fetch) {
        // texelFetch: nivel y coordenadas exactas; fuera de rango = 0
        const u32 level = t.min_level + u32(std::max(0, s32(r.lod)));
        if (level > t.max_level) { out[3] = 0; return; }
        const Level& lv = GetLevel(*tex, level);
        const s32 x = r.icoords[0] + r.offset[0], y = r.icoords[1] + r.offset[1];
        const s32 z = is3d ? r.icoords[2] + r.offset[2] : s32(r.layer);
        if (x < 0 || y < 0 || z < 0 || u32(x) >= lv.width || u32(y) >= lv.height || u32(z) >= lv.depth) {
            out[3] = 0;
            return;
        }
        Texel(lv, x, y, z, raw);
    } else {
        // Nivel de detalle. Sin derivadas (un pixel cada vez) el lod implicito es 0.
        float lod = (r.explicit_lod ? r.lod : 0.0f) + t.lod_bias + s.lod_bias + (r.bias ? r.lod : 0.0f);
        if (r.grad) {
            // textureGrad: lod = log2 del mayor cambio (en texeles) entre pixeles vecinos
            const Level& base = GetLevel(*tex, t.min_level);
            const float sz[3] = {float(base.width), float(base.height), is3d ? float(base.depth) : 0.0f};
            float lx = 0, ly = 0;
            for (int c = 0; c < 3; ++c) {
                lx += (r.ddx[c] * sz[c]) * (r.ddx[c] * sz[c]);
                ly += (r.ddy[c] * sz[c]) * (r.ddy[c] * sz[c]);
            }
            const float m = std::max(lx, ly);
            lod += m > 0 ? 0.5f * std::log2(m) : -100.0f;
        }
        lod = std::clamp(lod, s.min_lod, std::max(s.min_lod, s.max_lod));
        u32 level = t.min_level;
        if (s.mip_filter >= 2 && lod > 0)
            level = std::min(t.min_level + u32(s.mip_filter == 2 ? std::lround(lod) : std::floor(lod)), t.max_level);
        const bool linear = (lod > 0 ? s.min_filter : s.mag_filter) >= 2 && !tex->is_int;
        const Level& lv = GetLevel(*tex, level);
        const bool norm = t.normalized && !s.force_unnormalized;
        const float w = float(lv.width), h = float(lv.height), d = float(lv.depth);
        float u = r.coords[0] * (norm ? w : 1.0f) + float(r.offset[0]);
        float v = r.coords[1] * (norm ? h : 1.0f) + float(r.offset[1]);
        float q = is3d ? r.coords[2] * (norm ? d : 1.0f) + float(r.offset[2]) : 0.0f;
        const s32 layer = is3d ? 0 : std::clamp(s32(r.layer), 0, s32(lv.depth) - 1);
        auto fetch = [&](s32 x, s32 y, s32 z, u32 o[4]) {
            bool bx, by, bz = false;
            x = WrapInt(x, s.wrap[0], s32(lv.width), bx);
            y = WrapInt(y, s.wrap[1], s32(lv.height), by);
            if (is3d) z = WrapInt(z, s.wrap[2], s32(lv.depth), bz);
            if (bx || by || bz) { for (int c = 0; c < 4; ++c) o[c] = U(s.border[c]); return; }
            Texel(lv, x, y, z, o);
        };
        auto compare = [&](u32 o[4]) {   // muestreador "shadow": 1 si pasa la comparacion
            if (r.depth_compare) {
                const float pass = Compare(s.compare_func, r.dref, F(o[0])) ? 1.0f : 0.0f;
                o[0] = o[1] = o[2] = U(pass);
                o[3] = U(1.0f);
            }
        };
        if (!linear) {
            const s32 z = is3d ? s32(std::floor(q)) : layer;
            fetch(s32(std::floor(u)), s32(std::floor(v)), z, raw);
            compare(raw);
        } else {
            // Bilineal: los 4 texeles alrededor del punto (centros en +0.5)
            const float fu = u - 0.5f, fv = v - 0.5f;
            const s32 x0 = s32(std::floor(fu)), y0 = s32(std::floor(fv));
            const float ax = fu - float(x0), ay = fv - float(y0);
            const s32 z = is3d ? s32(std::floor(q)) : layer;
            float acc[4] = {};
            const float wts[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
            const s32 xs[4] = {x0, x0 + 1, x0, x0 + 1}, ys[4] = {y0, y0, y0 + 1, y0 + 1};
            for (int i = 0; i < 4; ++i) {
                u32 o[4];
                fetch(xs[i], ys[i], z, o);
                compare(o);
                for (int c = 0; c < 4; ++c) acc[c] += wts[i] * F(o[c]);
            }
            for (int c = 0; c < 4; ++c) raw[c] = U(acc[c]);
        }
    }

    // Reordenar componentes segun el descriptor (X, Y, Z, W <- R, G, B, A, 0, 1)
    for (int c = 0; c < 4; ++c) {
        switch (t.swizzle[c]) {
            case 2: out[c] = raw[0]; break;
            case 3: out[c] = raw[1]; break;
            case 4: out[c] = raw[2]; break;
            case 5: out[c] = raw[3]; break;
            case 6: out[c] = 1; break;
            case 7: out[c] = U(1.0f); break;
            default: out[c] = 0; break;
        }
    }
}

void TextureSampler::Query(u32 handle, u32 lod, u32 out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    Texture* tex = GetTexture(handle & 0xFFFFF);
    if (!tex) return;
    const TextureInfo& t = tex->info;
    const u32 level = t.min_level + lod;
    out[0] = std::max(t.width >> level, 1u);
    out[1] = std::max(t.height >> level, 1u);
    out[2] = t.type == 2 ? std::max(t.depth >> level, 1u) : t.depth;
    out[3] = t.max_level - t.min_level + 1;
}

} // namespace NeXo2::GPU
