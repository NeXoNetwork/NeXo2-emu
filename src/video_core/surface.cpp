#include "surface.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace NeXo2::GPU {

void MsaaSampleGrid(u32 mode, u32& sx, u32& sy) {
    switch (mode & 0xF) {
        case 1: case 5:          sx = 2; sy = 1; break;   // 2x1
        case 2: case 8: case 9:  sx = 2; sy = 2; break;   // 2x2
        case 3: case 4: case 10: case 11: sx = 4; sy = 2; break;   // 4x2
        case 6:                  sx = 4; sy = 4; break;   // 4x4
        default:                 sx = 1; sy = 1; break;
    }
}

u64 BlockLinearOffset(u32 x_bytes, u32 y, u32 width_bytes, u32 block_height_log2) {
    const u32 gob_rows = 8u << block_height_log2;            // filas por bloque
    const u32 blocks_per_row = (width_bytes + 63) / 64;       // un bloque mide 1 GOB (64 bytes) de ancho
    const u64 block_size = 512ull << block_height_log2;
    const u64 block = u64(y / gob_rows) * blocks_per_row + x_bytes / 64;
    const u32 gob_in_block = (y % gob_rows) / 8;
    // Dentro de un GOB (64 x 8 bytes): orden de los 512 bytes
    const u32 gx = x_bytes % 64, gy = y % 8;
    const u32 in_gob = ((gx % 64) / 32) * 256 + ((gy % 8) / 2) * 64 + ((gx % 32) / 16) * 32 + (gy % 2) * 16 + (gx % 16);
    return block * block_size + u64(gob_in_block) * 512 + in_gob;
}

namespace {
inline u32 Unorm(float v, u32 bits) {
    v = std::clamp(v, 0.0f, 1.0f);
    return u32(std::lround(v * float((1u << bits) - 1)));
}
inline u32 Snorm(float v, u32 bits) {
    v = std::clamp(v, -1.0f, 1.0f);
    const s32 m = s32((1u << (bits - 1)) - 1);
    return u32(s32(std::lround(v * float(m)))) & ((1u << bits) - 1);
}
inline float ToSrgb(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
inline u16 Half(float f) {   // float -> half (redondeo al mas cercano; suficiente para colores de borrado)
    u32 x; std::memcpy(&x, &f, 4);
    const u32 sign = (x >> 16) & 0x8000;
    s32 e = s32((x >> 23) & 0xFF) - 127 + 15;
    u32 m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return u16(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return u16(sign | 0x7C00);
    if (e <= 0) {
        if (e < -10) return u16(sign);
        m |= 0x800000;
        const u32 shift = u32(14 - e);
        u32 h = m >> shift;
        if ((m >> (shift - 1)) & 1) ++h;
        return u16(sign | h);
    }
    u32 h = sign | (u32(e) << 10) | (m >> 13);
    if (m & 0x1000) ++h;
    return u16(h);
}
template <typename T> void Put(u8* p, T v) { std::memcpy(p, &v, sizeof(T)); }
} // namespace

u32 ColorFormatBytes(u32 f) {
    switch (f) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 16;
        case 0xC6: case 0xC7: case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: return 8;
        case 0xCF: case 0xD0: case 0xD1: case 0xD2: case 0xD5: case 0xD6: case 0xD7: case 0xD8: case 0xD9:
        case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: case 0xE0: case 0xE3: case 0xE4:
        case 0xE5: case 0xE6: case 0xE7: case 0xF9: case 0xFA: case 0xFD: case 0xFE: case 0xFF: return 4;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF: case 0xF0:
        case 0xF1: case 0xF2: case 0xF8: case 0xFB: case 0xFC: return 2;
        case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7: return 1;
        default: return 0;
    }
}

bool EncodeColor(u32 f, const float c[4], u8* out) {
    const float r = c[0], g = c[1], b = c[2], a = c[3];
    auto u32bits = [](float v) { u32 x; std::memcpy(&x, &v, 4); return x; };
    switch (f) {
        // 128 bits: 4 x 32
        case 0xC0: case 0xC3: for (int i = 0; i < 4; ++i) Put<u32>(out + 4 * i, u32bits(c[i])); return true;
        case 0xC1: case 0xC4: for (int i = 0; i < 4; ++i) Put<s32>(out + 4 * i, s32(c[i])); return true;
        case 0xC2: case 0xC5: for (int i = 0; i < 4; ++i) Put<u32>(out + 4 * i, u32(std::max(c[i], 0.0f))); return true;
        // 64 bits: 4 x 16 o 2 x 32
        case 0xC6: for (int i = 0; i < 4; ++i) Put<u16>(out + 2 * i, u16(Unorm(c[i], 16))); return true;
        case 0xC7: for (int i = 0; i < 4; ++i) Put<u16>(out + 2 * i, u16(Snorm(c[i], 16))); return true;
        case 0xC8: for (int i = 0; i < 4; ++i) Put<s16>(out + 2 * i, s16(c[i])); return true;
        case 0xC9: for (int i = 0; i < 4; ++i) Put<u16>(out + 2 * i, u16(std::max(c[i], 0.0f))); return true;
        case 0xCA: case 0xCE: for (int i = 0; i < 4; ++i) Put<u16>(out + 2 * i, Half(c[i])); return true;
        case 0xCB: Put<u32>(out, u32bits(r)); Put<u32>(out + 4, u32bits(g)); return true;
        case 0xCC: Put<s32>(out, s32(r)); Put<s32>(out + 4, s32(g)); return true;
        case 0xCD: Put<u32>(out, u32(r)); Put<u32>(out + 4, u32(g)); return true;
        // 32 bits
        case 0xD5: case 0xF9: out[0] = u8(Unorm(r, 8)); out[1] = u8(Unorm(g, 8)); out[2] = u8(Unorm(b, 8)); out[3] = u8(Unorm(a, 8)); return true;
        case 0xD6: case 0xFA: out[0] = u8(Unorm(ToSrgb(r), 8)); out[1] = u8(Unorm(ToSrgb(g), 8)); out[2] = u8(Unorm(ToSrgb(b), 8)); out[3] = u8(Unorm(a, 8)); return true;
        case 0xD7: out[0] = u8(Snorm(r, 8)); out[1] = u8(Snorm(g, 8)); out[2] = u8(Snorm(b, 8)); out[3] = u8(Snorm(a, 8)); return true;
        case 0xD8: out[0] = u8(s8(r)); out[1] = u8(s8(g)); out[2] = u8(s8(b)); out[3] = u8(s8(a)); return true;
        case 0xD9: out[0] = u8(r); out[1] = u8(g); out[2] = u8(b); out[3] = u8(a); return true;
        case 0xCF: case 0xE6: case 0xFD: case 0xFE:
            out[0] = u8(Unorm(b, 8)); out[1] = u8(Unorm(g, 8)); out[2] = u8(Unorm(r, 8)); out[3] = u8(Unorm(a, 8)); return true;
        case 0xD0: case 0xE7:
            out[0] = u8(Unorm(ToSrgb(b), 8)); out[1] = u8(Unorm(ToSrgb(g), 8)); out[2] = u8(Unorm(ToSrgb(r), 8)); out[3] = u8(Unorm(a, 8)); return true;
        case 0xD1: Put<u32>(out, Unorm(r, 10) | (Unorm(g, 10) << 10) | (Unorm(b, 10) << 20) | (Unorm(a, 2) << 30)); return true;
        case 0xDF: Put<u32>(out, Unorm(b, 10) | (Unorm(g, 10) << 10) | (Unorm(r, 10) << 20) | (Unorm(a, 2) << 30)); return true;
        case 0xD2: Put<u32>(out, (u32(r) & 0x3FF) | ((u32(g) & 0x3FF) << 10) | ((u32(b) & 0x3FF) << 20) | ((u32(a) & 3) << 30)); return true;
        case 0xDA: Put<u16>(out, u16(Unorm(r, 16))); Put<u16>(out + 2, u16(Unorm(g, 16))); return true;
        case 0xDB: Put<u16>(out, u16(Snorm(r, 16))); Put<u16>(out + 2, u16(Snorm(g, 16))); return true;
        case 0xDC: Put<s16>(out, s16(r)); Put<s16>(out + 2, s16(g)); return true;
        case 0xDD: Put<u16>(out, u16(r)); Put<u16>(out + 2, u16(g)); return true;
        case 0xDE: Put<u16>(out, Half(r)); Put<u16>(out + 2, Half(g)); return true;
        case 0xE3: Put<s32>(out, s32(r)); return true;
        case 0xE4: case 0xFF: Put<u32>(out, u32(r)); return true;
        case 0xE5: Put<u32>(out, u32bits(r)); return true;
        case 0xE0: {   // R11G11B10F: aproximado desde half (quitando bits bajos)
            const u32 hr = Half(std::max(r, 0.0f)), hg = Half(std::max(g, 0.0f)), hb = Half(std::max(b, 0.0f));
            Put<u32>(out, ((hr >> 4) & 0x7FF) | (((hg >> 4) & 0x7FF) << 11) | (((hb >> 5) & 0x3FF) << 22));
            return true;
        }
        // 16 bits
        case 0xE8: Put<u16>(out, u16(Unorm(r, 5) | (Unorm(g, 6) << 5) | (Unorm(b, 5) << 11))); return true;
        case 0xE9: Put<u16>(out, u16(Unorm(r, 5) | (Unorm(g, 5) << 5) | (Unorm(b, 5) << 10) | (Unorm(a, 1) << 15))); return true;
        case 0xF8: case 0xFB: case 0xFC: Put<u16>(out, u16(Unorm(r, 5) | (Unorm(g, 5) << 5) | (Unorm(b, 5) << 10) | 0x8000)); return true;
        case 0xEA: out[0] = u8(Unorm(r, 8)); out[1] = u8(Unorm(g, 8)); return true;
        case 0xEB: out[0] = u8(Snorm(r, 8)); out[1] = u8(Snorm(g, 8)); return true;
        case 0xEC: out[0] = u8(s8(r)); out[1] = u8(s8(g)); return true;
        case 0xED: out[0] = u8(r); out[1] = u8(g); return true;
        case 0xEE: Put<u16>(out, u16(Unorm(r, 16))); return true;
        case 0xEF: Put<u16>(out, u16(Snorm(r, 16))); return true;
        case 0xF0: Put<s16>(out, s16(r)); return true;
        case 0xF1: Put<u16>(out, u16(r)); return true;
        case 0xF2: Put<u16>(out, Half(r)); return true;
        // 8 bits
        case 0xF3: out[0] = u8(Unorm(r, 8)); return true;
        case 0xF4: out[0] = u8(Snorm(r, 8)); return true;
        case 0xF5: out[0] = u8(s8(r)); return true;
        case 0xF6: out[0] = u8(r); return true;
        case 0xF7: out[0] = u8(Unorm(a, 8)); return true;
        default: return false;
    }
}

u32 ColorWriteByteMask(u32 f, u32 m) {
    const u32 bytes = ColorFormatBytes(f);
    const u32 all = (1u << bytes) - 1;
    if ((m & 0xF) == 0xF) return all;
    auto comp = [&](u32 size) {   // componentes de 'size' bytes seguidos en orden R,G,B,A
        u32 out = 0;
        for (u32 i = 0; i < 4 && (i + 1) * size <= bytes; ++i)
            if (m & (1u << i)) out |= ((1u << size) - 1) << (i * size);
        return out;
    };
    switch (f) {
        case 0xD5: case 0xD6: case 0xD7: case 0xD8: case 0xD9: case 0xF9: case 0xFA:
        case 0xEA: case 0xEB: case 0xEC: case 0xED: return comp(1);
        case 0xCF: case 0xD0: case 0xE6: case 0xE7: case 0xFD: case 0xFE: {   // B,G,R,A
            u32 out = 0;
            if (m & 1) out |= 4;
            if (m & 2) out |= 2;
            if (m & 4) out |= 1;
            if (m & 8) out |= 8;
            return out;
        }
        case 0xC6: case 0xC7: case 0xC8: case 0xC9: case 0xCA: case 0xCE:
        case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: return comp(2);
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xCB: case 0xCC: case 0xCD: return comp(4);
        default: return (m & 1) ? all : 0;   // formatos de un componente o empaquetados
    }
}

u32 DepthFormatBytes(u32 f) {
    switch (f) {
        case 0x13: return 2;                       // Z16
        case 0x17: return 1;                       // S8
        case 0x0A: case 0x14: case 0x15: case 0x16: case 0x18: return 4;
        case 0x19: return 8;                       // Z32F + S8 (+24 de relleno)
        default: return 0;
    }
}

bool EncodeDepthStencil(u32 f, float depth, u8 stencil, bool wd, bool ws, u8* px) {
    depth = std::clamp(depth, 0.0f, 1.0f);
    const u32 z24 = Unorm(depth, 24);
    auto rd32 = [&] { u32 v; std::memcpy(&v, px, 4); return v; };
    switch (f) {
        case 0x0A: if (wd) std::memcpy(px, &depth, 4); return true;                     // Z32F
        case 0x13: if (wd) Put<u16>(px, u16(Unorm(depth, 16))); return true;            // Z16
        case 0x17: if (ws) px[0] = stencil; return true;                                // S8
        case 0x14: case 0x18: {    // S8Z24: Z en los bits 0..23, S en 24..31
            u32 v = rd32();
            if (wd) v = (v & 0xFF000000u) | z24;
            if (ws) v = (v & 0x00FFFFFFu) | (u32(stencil) << 24);
            Put<u32>(px, v);
            return true;
        }
        case 0x15: if (wd) Put<u32>(px, z24); return true;                              // Z24X8
        case 0x16: {               // Z24S8: S en los bits 0..7, Z en 8..31
            u32 v = rd32();
            if (wd) v = (v & 0xFFu) | (z24 << 8);
            if (ws) v = (v & ~0xFFu) | stencil;
            Put<u32>(px, v);
            return true;
        }
        case 0x19:                 // Z32F_X24S8
            if (wd) std::memcpy(px, &depth, 4);
            if (ws) px[4] = stencil;
            return true;
        default: return false;
    }
}

namespace {
inline float FromUnorm(u32 v, u32 bits) { return float(v) / float((1u << bits) - 1); }
inline float FromSnorm(u32 v, u32 bits) {
    const s32 sv = s32(v << (32 - bits)) >> (32 - bits);
    return std::max(float(sv) / float((1u << (bits - 1)) - 1), -1.0f);
}
inline float FromSrgb(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
inline float FromHalf(u16 h) {
    const u32 sign = u32(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    u32 x;
    if (e == 0) {
        if (m == 0) x = sign;
        else { const float f = std::ldexp(float(m), -24); return sign ? -f : f; }
    } else if (e == 31) x = sign | 0x7F800000u | (m << 13);
    else x = sign | ((e + 112) << 23) | (m << 13);
    float f; std::memcpy(&f, &x, 4); return f;
}
template <typename T> T Get(const u8* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
} // namespace

bool DecodeColor(u32 f, const u8* p, float c[4]) {
    c[0] = c[1] = c[2] = 0.0f; c[3] = 1.0f;
    switch (f) {
        case 0xC0: case 0xC3: for (int i = 0; i < 4; ++i) c[i] = Get<float>(p + 4 * i); return true;
        case 0xC6: for (int i = 0; i < 4; ++i) c[i] = FromUnorm(Get<u16>(p + 2 * i), 16); return true;
        case 0xC7: for (int i = 0; i < 4; ++i) c[i] = FromSnorm(Get<u16>(p + 2 * i), 16); return true;
        case 0xCA: case 0xCE: for (int i = 0; i < 4; ++i) c[i] = FromHalf(Get<u16>(p + 2 * i)); return true;
        case 0xCB: c[0] = Get<float>(p); c[1] = Get<float>(p + 4); return true;
        case 0xD5: case 0xF9: for (int i = 0; i < 4; ++i) c[i] = FromUnorm(p[i], 8); return true;
        case 0xD6: case 0xFA: for (int i = 0; i < 3; ++i) c[i] = FromSrgb(FromUnorm(p[i], 8)); c[3] = FromUnorm(p[3], 8); return true;
        case 0xD7: for (int i = 0; i < 4; ++i) c[i] = FromSnorm(p[i], 8); return true;
        case 0xCF: case 0xE6: case 0xFD: case 0xFE:
            c[0] = FromUnorm(p[2], 8); c[1] = FromUnorm(p[1], 8); c[2] = FromUnorm(p[0], 8); c[3] = FromUnorm(p[3], 8); return true;
        case 0xD0: case 0xE7:
            c[0] = FromSrgb(FromUnorm(p[2], 8)); c[1] = FromSrgb(FromUnorm(p[1], 8)); c[2] = FromSrgb(FromUnorm(p[0], 8)); c[3] = FromUnorm(p[3], 8); return true;
        case 0xD1: { const u32 v = Get<u32>(p); c[0] = FromUnorm(v & 0x3FF, 10); c[1] = FromUnorm((v >> 10) & 0x3FF, 10); c[2] = FromUnorm((v >> 20) & 0x3FF, 10); c[3] = FromUnorm(v >> 30, 2); return true; }
        case 0xDF: { const u32 v = Get<u32>(p); c[2] = FromUnorm(v & 0x3FF, 10); c[1] = FromUnorm((v >> 10) & 0x3FF, 10); c[0] = FromUnorm((v >> 20) & 0x3FF, 10); c[3] = FromUnorm(v >> 30, 2); return true; }
        case 0xDA: c[0] = FromUnorm(Get<u16>(p), 16); c[1] = FromUnorm(Get<u16>(p + 2), 16); return true;
        case 0xDB: c[0] = FromSnorm(Get<u16>(p), 16); c[1] = FromSnorm(Get<u16>(p + 2), 16); return true;
        case 0xDE: c[0] = FromHalf(Get<u16>(p)); c[1] = FromHalf(Get<u16>(p + 2)); return true;
        case 0xE5: c[0] = Get<float>(p); return true;
        case 0xE8: { const u16 v = Get<u16>(p); c[0] = FromUnorm(v & 31, 5); c[1] = FromUnorm((v >> 5) & 63, 6); c[2] = FromUnorm(v >> 11, 5); return true; }
        case 0xE9: { const u16 v = Get<u16>(p); c[0] = FromUnorm(v & 31, 5); c[1] = FromUnorm((v >> 5) & 31, 5); c[2] = FromUnorm((v >> 10) & 31, 5); c[3] = float(v >> 15); return true; }
        case 0xF8: case 0xFB: case 0xFC: { const u16 v = Get<u16>(p); c[0] = FromUnorm(v & 31, 5); c[1] = FromUnorm((v >> 5) & 31, 5); c[2] = FromUnorm((v >> 10) & 31, 5); return true; }
        case 0xEA: c[0] = FromUnorm(p[0], 8); c[1] = FromUnorm(p[1], 8); return true;
        case 0xEB: c[0] = FromSnorm(p[0], 8); c[1] = FromSnorm(p[1], 8); return true;
        case 0xEE: c[0] = FromUnorm(Get<u16>(p), 16); return true;
        case 0xEF: c[0] = FromSnorm(Get<u16>(p), 16); return true;
        case 0xF2: c[0] = FromHalf(Get<u16>(p)); return true;
        case 0xF3: c[0] = FromUnorm(p[0], 8); return true;
        case 0xF4: c[0] = FromSnorm(p[0], 8); return true;
        case 0xF7: c[3] = FromUnorm(p[0], 8); return true;
        default: return false;   // formatos enteros (no se mezclan) y raros
    }
}

bool DecodeDepth(u32 f, const u8* px, float& depth) {
    switch (f) {
        case 0x0A: case 0x19: depth = Get<float>(px); return true;                         // Z32F
        case 0x13: depth = FromUnorm(Get<u16>(px), 16); return true;                        // Z16
        case 0x14: case 0x15: case 0x18: depth = FromUnorm(Get<u32>(px) & 0xFFFFFF, 24); return true;   // S8Z24 / X8Z24
        case 0x16: depth = FromUnorm(Get<u32>(px) >> 8, 24); return true;                   // Z24S8
        default: return false;
    }
}

} // namespace NeXo2::GPU
