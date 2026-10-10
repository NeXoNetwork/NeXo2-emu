// Dibujo por software (ver rasterizer.hpp y docs/07-nexo-internals/gpu-rasterizer.md).
#include "rasterizer.hpp"
#include "texture.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace NeXo2::GPU {

namespace {

// --- Registros del motor 3D que usamos (direccion del metodo / 4) ---
constexpr u32 RT_BASE = 0x200;                 // render targets, 0x10 registros cada uno
constexpr u32 VIEWPORT_XFORM = 0x280;          // 8 cada uno: escala x,y,z y desplazamiento x,y,z
constexpr u32 VIEWPORT = 0x300;                // 4 cada uno: horizontal, vertical, cerca, lejos
constexpr u32 DEPTH_MODE = 0x35F;              // 0 = z de -w a w (OpenGL), 1 = z de 0 a w
constexpr u32 SCISSOR = 0x380;                 // 4 cada uno: activo, horizontal, vertical
constexpr u32 ZT_ADDR = 0x3F8, ZT_FORMAT = 0x3FA, ZT_BLOCK = 0x3FB;
constexpr u32 SCREEN_SCISSOR_H = 0x3FD, SCREEN_SCISSOR_V = 0x3FE;
constexpr u32 SINGLE_CT_WRITE = 0x3E4, MRT_ENABLE = 0x3EB;
constexpr u32 VERTEX_ATTRIB = 0x458;           // 32 atributos de vertice
constexpr u32 RT_CONTROL = 0x487;              // cuantos render targets y cuales
constexpr u32 ZT_WIDTH = 0x48A, ZT_HEIGHT = 0x48B;
constexpr u32 DEPTH_TEST = 0x4B3, BLEND_PER_TARGET = 0x4B9, DEPTH_WRITE = 0x4BA, DEPTH_FUNC = 0x4C3;
constexpr u32 BLEND_CONST = 0x4C7;
constexpr u32 BLEND_GLOBAL = 0x4CF;            // separado, op color, origen, destino, op alfa, origen, -, destino
constexpr u32 BLEND_ENABLE = 0x4D8;            // 8 render targets
constexpr u32 WINDOW_ORIGIN = 0x4EB;
constexpr u32 ZT_SELECT = 0x54E;
constexpr u32 PROGRAM_REGION = 0x582;
constexpr u32 PRIM_RESTART = 0x591, PRIM_RESTART_INDEX = 0x592;
constexpr u32 PROVOKING_LAST = 0x5A1;
constexpr u32 INDEX_ADDR = 0x5F2, INDEX_FORMAT = 0x5F6;
constexpr u32 STREAM_PER_INSTANCE = 0x620;
constexpr u32 CULL_ENABLE = 0x646, FRONT_FACE = 0x647, CULL_FACE = 0x648;
constexpr u32 VIEWPORT_XFORM_ENABLE = 0x64B;
constexpr u32 CT_WRITE = 0x680;
constexpr u32 VERTEX_STREAM = 0x700;           // 4 cada uno: config, direccion (2), divisor
constexpr u32 BLEND_TARGET = 0x780;            // 8 cada uno
constexpr u32 PIPELINE = 0x800;                // 0x10 cada uno: config, offset, -, registros, grupo
constexpr u32 TSC_POOL = 0x557, TSC_MAX = 0x559;   // tabla de samplers: direccion (2), ultimo indice
constexpr u32 TIC_POOL = 0x55D, TIC_MAX = 0x55F;   // tabla de imagenes
constexpr u32 BINDLESS_TEXTURE = 0x982;
constexpr u32 MULTISAMPLE_MODE = 0x574;           // MSAA: cuantas muestras por pixel            // constbuf con los handles de texturas

inline u64 Iova(u32 hi, u32 lo) { return (u64(hi & 0xFF) << 32) | lo; }
inline float F(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
inline u32 U(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }

float HalfToFloat(u16 h) {
    const u32 sign = u32(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0) { const float f = std::ldexp(float(m), -24); return sign ? -f : f; }
    if (e == 31) return F(sign | 0x7F800000u | (m << 13));
    return F(sign | ((e + 112) << 23) | (m << 13));
}

// Lineas de un render target en memoria (como en engines.cpp: el block linear se
// escribe en trozos de 16 bytes, que dentro de un GOB son seguidos)
// Posicion dentro de un GOB del byte 'o' (0..63) de una fila (la parte de la fila ya va en la base)
inline u32 GobColumn(u32 o) { return (o / 32) * 256 + ((o % 32) / 16) * 32 + (o % 16); }

// Copia la parte [gx, gx+n) de la fila 'y' de un GOB. Camino rapido: el GOB (512 bytes
// seguidos) esta en una pagina de memoria y se copia directamente; si no, de 16 en 16 bytes.
template <bool Write>
void GobSpan(GpuMemoryManager& mm, const Surface& s, u32 xb, u32 y, u32 n, u8* buf) {
    const u32 gx = xb % 64;
    const u64 row = s.address + BlockLinearOffset(xb - gx, y, s.width * s.bytes_per_pixel, s.block_height_log2);
    u8* gob = (s.address & 511) == 0 ? mm.HostPointer(row & ~511ull, 512, Write) : nullptr;
    if (gob) {
        u8* base = gob + (row & 511);
        for (u32 o = gx; o < gx + n;) {
            const u32 piece = std::min(16 - (o % 16), gx + n - o);
            if constexpr (Write) std::memcpy(base + GobColumn(o), buf, piece);
            else std::memcpy(buf, base + GobColumn(o), piece);
            o += piece; buf += piece;
        }
        return;
    }
    for (u32 o = gx; o < gx + n;) {
        const u32 piece = std::min(16 - (o % 16), gx + n - o);
        if constexpr (Write) mm.WriteBlock(row + GobColumn(o), buf, piece);
        else mm.ReadBlock(row + GobColumn(o), buf, piece);
        o += piece; buf += piece;
    }
}

void ReadSpan(GpuMemoryManager& mm, const Surface& s, u32 x, u32 y, u32 count, u8* out) {
    const u32 bpp = s.bytes_per_pixel;
    u32 xb = x * bpp, size = count * bpp;
    if (s.linear) { mm.ReadBlock(s.address + u64(y) * s.pitch + xb, out, size); return; }
    while (size > 0) {
        const u32 n = std::min(size, 64 - (xb % 64));   // hasta el final del GOB
        GobSpan<false>(mm, s, xb, y, n, out);
        xb += n; out += n; size -= n;
    }
}
void WriteSpan(GpuMemoryManager& mm, const Surface& s, u32 x, u32 y, u32 count, const u8* in);

// Con MSAA la superficie guarda msx x msy muestras por pixel (como una imagen msx veces mas
// ancha y msy veces mas alta). No calculamos la cobertura por muestra: cada pixel se lee de
// su primera muestra y se escribe en todas (sin bordes suavizados, pero el "resolve" que
// hace el programa despues da el mismo color).
void ReadPixels(GpuMemoryManager& mm, const Surface& s, u32 msx, u32 msy, u32 x, u32 y, u32 count, u8* out,
                std::vector<u8>& tmp) {
    if (msx == 1 && msy == 1) { ReadSpan(mm, s, x, y, count, out); return; }
    const u32 bpp = s.bytes_per_pixel;
    tmp.resize(size_t(count) * msx * bpp);
    ReadSpan(mm, s, x * msx, y * msy, count * msx, tmp.data());
    for (u32 i = 0; i < count; ++i) std::memcpy(out + size_t(i) * bpp, tmp.data() + size_t(i) * msx * bpp, bpp);
}
void WritePixels(GpuMemoryManager& mm, const Surface& s, u32 msx, u32 msy, u32 x, u32 y, u32 count, const u8* in,
                 std::vector<u8>& tmp) {
    if (msx == 1 && msy == 1) { WriteSpan(mm, s, x, y, count, in); return; }
    const u32 bpp = s.bytes_per_pixel;
    tmp.resize(size_t(count) * msx * bpp);
    for (u32 i = 0; i < count; ++i)
        for (u32 k = 0; k < msx; ++k) std::memcpy(tmp.data() + (size_t(i) * msx + k) * bpp, in + size_t(i) * bpp, bpp);
    for (u32 r = 0; r < msy; ++r) WriteSpan(mm, s, x * msx, y * msy + r, count * msx, tmp.data());
}
void WriteSpan(GpuMemoryManager& mm, const Surface& s, u32 x, u32 y, u32 count, const u8* in) {
    const u32 bpp = s.bytes_per_pixel;
    u32 xb = x * bpp, size = count * bpp;
    if (s.linear) { mm.WriteBlock(s.address + u64(y) * s.pitch + xb, in, size); return; }
    while (size > 0) {
        const u32 n = std::min(size, 64 - (xb % 64));
        GobSpan<true>(mm, s, xb, y, n, const_cast<u8*>(in));
        xb += n; in += n; size -= n;
    }
}

// Funcion de comparacion (profundidad): 0 nunca .. 7 siempre (valores OpenGL 0x200.. o D3D 1..)
bool CompareFunc(u32 func, float a, float b) {
    const u32 f = func >= 0x200 ? func - 0x200 : func - 1;
    switch (f & 7) {
        case 0: return false;
        case 1: return a < b;
        case 2: return a == b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return a != b;
        case 6: return a >= b;
        default: return true;
    }
}

// Factor de mezcla (blending). Acepta los valores de OpenGL y de Direct3D.
float BlendFactor(u32 f, const float src[4], const float dst[4], const float cst[4], int c) {
    switch (f) {
        case 0x4000: case 0x01: return 0.0f;
        case 0x4001: case 0x02: return 1.0f;
        case 0x4300: case 0x03: case 0xC900: case 0x10: return src[c];
        case 0x4301: case 0x04: case 0xC901: case 0x11: return 1.0f - src[c];
        case 0x4302: case 0x05: case 0xC902: case 0x12: return src[3];
        case 0x4303: case 0x06: case 0xC903: case 0x13: return 1.0f - src[3];
        case 0x4304: case 0x07: return dst[3];
        case 0x4305: case 0x08: return 1.0f - dst[3];
        case 0x4306: case 0x09: return dst[c];
        case 0x4307: case 0x0A: return 1.0f - dst[c];
        case 0x4308: case 0x0B: return c == 3 ? 1.0f : std::min(src[3], 1.0f - dst[3]);
        case 0xC001: case 0x0E: return cst[c];
        case 0xC002: case 0x0F: return 1.0f - cst[c];
        case 0xC003: return cst[3];
        case 0xC004: return 1.0f - cst[3];
        default: return 1.0f;
    }
}
float BlendEquation(u32 op, float s, float d, float fs, float fd) {
    switch (op) {
        case 0x800A: case 0x02: return s * fs - d * fd;
        case 0x800B: case 0x03: return d * fd - s * fs;
        case 0x8007: case 0x04: return std::min(s, d);
        case 0x8008: case 0x05: return std::max(s, d);
        default:                return s * fs + d * fd;   // ADD
    }
}

// Formato de los atributos de vertice: cuantos componentes y de cuantos bits
struct AttribLayout { u32 count; u32 bits[4]; u32 bytes; bool packed; };
bool GetAttribLayout(u32 size, AttribLayout& l) {
    switch (size) {
        case 0x01: l = {4, {32, 32, 32, 32}, 16, false}; return true;
        case 0x02: l = {3, {32, 32, 32, 0}, 12, false}; return true;
        case 0x04: l = {2, {32, 32, 0, 0}, 8, false}; return true;
        case 0x12: l = {1, {32, 0, 0, 0}, 4, false}; return true;
        case 0x03: l = {4, {16, 16, 16, 16}, 8, false}; return true;
        case 0x05: l = {3, {16, 16, 16, 0}, 6, false}; return true;
        case 0x0F: l = {2, {16, 16, 0, 0}, 4, false}; return true;
        case 0x1B: l = {1, {16, 0, 0, 0}, 2, false}; return true;
        case 0x0A: case 0x2F: l = {4, {8, 8, 8, 8}, 4, false}; return true;
        case 0x13: l = {3, {8, 8, 8, 0}, 3, false}; return true;
        case 0x33: l = {3, {8, 8, 8, 0}, 4, false}; return true;
        case 0x18: case 0x32: l = {2, {8, 8, 0, 0}, 2, false}; return true;
        case 0x1D: l = {1, {8, 0, 0, 0}, 1, false}; return true;
        case 0x30: l = {4, {10, 10, 10, 2}, 4, true}; return true;
        default: return false;
    }
}

} // namespace

float SoftwareRasterizer::Vertex::Pos(u32 i) const { return F(attr[0x70 / 4 + i]); }

// ============================================================================
//  Estado de un draw
// ============================================================================

struct SoftwareRasterizer::DrawState {
    const u32* regs;
    const ConstbufTable* cbs;
    const DrawCall* dc;
    GpuMemoryManager* mm;
    std::unique_ptr<ShaderProgram> vs, fs;
    u32 vs_group = 0, fs_group = 4;
    // Constbufs copiados a memoria la primera vez que se leen
    std::array<std::array<std::vector<u32>, 18>, 5> cb_cache;
    std::array<std::array<bool, 18>, 5> cb_loaded{};
    // Texturas (descriptores leidos y niveles decodificados, solo durante este draw)
    std::unique_ptr<TextureSampler> textures;
    u32 tex_cb = 0;
    // Render targets
    struct Target {
        Surface surf;
        u32 format = 0;
        u32 index = 0;          // RT fisico (0..7)
        u32 write_mask = 0xF;   // RGBA
        bool blend = false;
        u32 blend_regs[7] = {}; // separado, op, origen, destino, op alfa, origen alfa, destino alfa
    };
    std::vector<Target> targets;
    bool mrt = true;
    // Profundidad
    bool depth_enabled = false, depth_test = false, depth_write = false;
    Surface zsurf;
    u32 zformat = 0, depth_func = 0;
    // Recorte
    u32 bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    // Salidas del shader de pixeles: registro de cada componente (-1 = no lo escribe)
    std::array<std::array<s32, 4>, 8> out_reg{};
    s32 depth_reg = -1;
    bool flip_y = false;
    u32 msx = 1, msy = 1;         // muestras por pixel (MSAA) en horizontal y vertical
    float y_direction = 1.0f;
    ShaderInterpreter interp;
    std::deque<Vertex> scratch;   // vertices creados al recortar
    std::vector<RasterTri> tris;  // triangulos del draw, ya recortados y en pantalla
    u64 pixels = 0, triangles = 0;
    bool failed = false;
    std::string error;

    u32 ReadConst(u32 stage, u32 index, u32 offset) {
        if (stage >= 5 || index >= 18) return 0;
        if (!cb_loaded[stage][index]) {
            cb_loaded[stage][index] = true;
            const ConstbufBinding& b = (*cbs)[stage][index];
            if (b.valid && b.size) {
                auto& v = cb_cache[stage][index];
                v.resize((std::min<u32>(b.size, 0x10000) + 3) / 4);
                mm->ReadBlock(b.address, v.data(), v.size() * 4);
            }
        }
        const auto& v = cb_cache[stage][index];
        return offset / 4 < v.size() ? v[offset / 4] : 0;
    }
};

// ============================================================================
//  Shader de vertices
// ============================================================================

namespace {
class VertexEnv final : public ShaderEnv {
public:
    VertexEnv(const u32* regs, GpuMemoryManager& mm, const DrawCall& dc, u32 index, u32 vertex_id,
              std::function<u32(u32, u32)> read_const, SoftwareRasterizer::Vertex& out)
        : m_regs(regs), m_mm(mm), m_dc(dc), m_index(index), m_vertex_id(vertex_id),
          m_readConst(std::move(read_const)), m_out(out) {}

    u32 ReadConst(u32 index, u32 offset) override { return m_readConst(index, offset); }
    u32 ReadAttribute(u32 addr) override {
        if (addr >= 0x80 && addr < 0x280) {
            const u32 loc = (addr - 0x80) / 16, comp = (addr / 4) & 3;
            if (!(m_fetched & (1u << loc))) { Fetch(loc); m_fetched |= 1u << loc; }
            return m_attr[loc][comp];
        }
        if (addr == 0x2FC) return m_vertex_id;
        if (addr == 0x2F8) return m_dc.instance;
        return 0;
    }
    void WriteAttribute(u32 addr, u32 value) override { if (addr < 0x400) m_out.attr[addr / 4] = value; }
    float Interpolate(u32, u32) override { return 0.0f; }
    void SampleTexture(u32 handle, const TextureRequest& req, u32 out[4]) override {
        TextureRequest r = req;
        if (!r.fetch && !r.explicit_lod) { r.explicit_lod = true; r.lod = 0; }   // sin derivadas: nivel 0
        tex->Sample(handle, r, out);
    }
    void QueryTexture(u32 handle, u32 lod, u32 out[4]) override { tex->Query(handle, lod, out); }
    u32 TextureConstbuf() override { return tex_cb; }
    TextureSampler* tex = nullptr;
    u32 tex_cb = 0;

private:
    void Fetch(u32 loc) {
        const u32 a = m_regs[VERTEX_ATTRIB + loc];
        const u32 stream = a & 0x1F, offset = (a >> 7) & 0x3FFF, size = (a >> 21) & 0x3F, type = (a >> 27) & 7;
        const bool is_int = type == 3 || type == 4;
        u32* out = m_attr[loc];
        out[0] = out[1] = out[2] = 0;
        out[3] = is_int ? 1 : U(1.0f);
        AttribLayout l;
        if (((a >> 6) & 1) || stream >= 16 || !GetAttribLayout(size, l)) return;   // constante: (0,0,0,1)
        const u32 cfg = m_regs[VERTEX_STREAM + 4 * stream];
        if (!((cfg >> 12) & 1)) return;
        const u32 stride = cfg & 0xFFF;
        u64 addr = Iova(m_regs[VERTEX_STREAM + 4 * stream + 1], m_regs[VERTEX_STREAM + 4 * stream + 2]);
        u32 element = m_index;
        if (m_regs[STREAM_PER_INSTANCE + stream] & 1) {
            const u32 div = m_regs[VERTEX_STREAM + 4 * stream + 3];
            element = m_dc.base_instance + (div ? m_dc.instance / div : m_dc.instance);
        }
        addr += u64(element) * stride + offset;
        u8 raw[16] = {};
        m_mm.ReadBlock(addr, raw, l.bytes);
        u32 comps[4] = {};
        if (l.packed) {
            u32 v; std::memcpy(&v, raw, 4);
            comps[0] = v & 0x3FF; comps[1] = (v >> 10) & 0x3FF; comps[2] = (v >> 20) & 0x3FF; comps[3] = v >> 30;
        } else {
            u32 bytepos = 0;
            for (u32 i = 0; i < l.count; ++i) {
                u32 v = 0;
                std::memcpy(&v, raw + bytepos, l.bits[i] / 8);
                comps[i] = v;
                bytepos += l.bits[i] / 8;
            }
        }
        for (u32 i = 0; i < l.count; ++i) {
            const u32 bits = l.bits[i], v = comps[i];
            const s32 sv = bits < 32 ? s32(v << (32 - bits)) >> (32 - bits) : s32(v);
            switch (type) {
                case 1: out[i] = U(std::max(float(sv) / float((1u << (bits - 1)) - 1), -1.0f)); break;   // snorm
                case 2: out[i] = U(bits >= 32 ? float(v) / 4294967295.0f : float(v) / float((1u << bits) - 1)); break;   // unorm
                case 3: out[i] = u32(sv); break;                       // sint
                case 4: out[i] = v; break;                             // uint
                case 5: out[i] = U(float(v)); break;                   // uscaled
                case 6: out[i] = U(float(sv)); break;                  // sscaled
                default:                                               // float
                    out[i] = bits == 32 ? v : bits == 16 ? U(HalfToFloat(u16(v))) : U(float(v));
                    break;
            }
        }
        if (size == 0x33) out[3] = is_int ? 1 : U(1.0f);   // X8B8G8R8: sin alfa
        if (a >> 31) std::swap(out[0], out[2]);              // BGRA
    }

    const u32* m_regs;
    GpuMemoryManager& m_mm;
    const DrawCall& m_dc;
    u32 m_index, m_vertex_id;
    std::function<u32(u32, u32)> m_readConst;
    SoftwareRasterizer::Vertex& m_out;
    u32 m_fetched = 0;
    u32 m_attr[32][4] = {};
};
} // namespace

bool SoftwareRasterizer::RunVertexShader(DrawState& st, u32 index, u32 vertex_id, Vertex& out) {
    out.attr.fill(0);
    out.attr[0x7C / 4] = U(1.0f);
    VertexEnv env(st.regs, *st.mm, *st.dc, index, vertex_id,
                  [&st](u32 i, u32 o) { return st.ReadConst(st.vs_group, i, o); }, out);
    env.tex = st.textures.get();
    env.tex_cb = st.tex_cb;
    const auto res = st.interp.Run(*st.vs, env);
    if (!res.ok) { st.failed = true; st.error = "shader de vertices: " + res.error; }
    return res.ok;
}

// ============================================================================
//  Draw
// ============================================================================

// ============================================================================
//  Hilos: cada uno se queda con unas filas de la pantalla (bandas de 4 filas
//  alternas) y recorre TODOS los triangulos del draw en orden. Asi cada pixel lo
//  escribe siempre el mismo hilo, en el mismo orden que en la GPU: la profundidad y
//  la mezcla dan exactamente lo mismo que con un solo hilo.
// ============================================================================

class SoftwareRasterizer::ThreadPool {
public:
    explicit ThreadPool(u32 threads) {
        for (u32 i = 1; i < threads; ++i) m_threads.emplace_back([this, i] { Loop(i); });
    }
    ~ThreadPool() {
        { std::lock_guard lock(m_mutex); m_quit = true; }
        m_wake.notify_all();
        for (auto& t : m_threads) t.join();
    }
    u32 Size() const { return u32(m_threads.size()) + 1; }
    // Ejecuta job(0..n-1): el 0 en este hilo, el resto en los otros. Espera a que acaben.
    void Run(u32 n, const std::function<void(u32)>& job) {
        n = std::min(n, Size());
        {
            std::lock_guard lock(m_mutex);
            m_job = &job;
            m_count = n;
            m_pending = n - 1;
            ++m_round;
        }
        m_wake.notify_all();
        job(0);
        std::unique_lock lock(m_mutex);
        m_done.wait(lock, [this] { return m_pending == 0; });
        m_job = nullptr;
    }
private:
    void Loop(u32 index) {
        u64 seen = 0;
        while (true) {
            const std::function<void(u32)>* job;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [&] { return m_quit || m_round != seen; });
                if (m_quit) return;
                seen = m_round;
                if (index >= m_count) continue;   // esta vez no hace falta este hilo
                job = m_job;
            }
            (*job)(index);
            std::lock_guard lock(m_mutex);
            if (--m_pending == 0) m_done.notify_one();
        }
    }
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake, m_done;
    const std::function<void(u32)>* m_job = nullptr;
    u32 m_count = 0, m_pending = 0;
    u64 m_round = 0;
    bool m_quit = false;
};

struct SoftwareRasterizer::Worker {
    ShaderInterpreter interp;
    std::unique_ptr<ShaderProgram> fs;          // cada hilo decodifica su copia
    TextureSampler* tex = nullptr;
    std::unique_ptr<TextureSampler> own_tex;    // texturas decodificadas por este hilo
    std::vector<u8> ms_tmp;
    u64 fs_steps = 0;
    std::vector<std::vector<u8>> rows;
    std::vector<u8> zrow;
    u64 pixels = 0;
    bool failed = false;
    std::string error;
};

namespace {
u32 GpuThreads() {
    // NEXO2_GPU_THREADS=1 para un solo hilo (comparar o depurar)
    if (const char* e = std::getenv("NEXO2_GPU_THREADS")) return std::clamp(std::atoi(e), 1, 64);
    return std::clamp<u32>(std::thread::hardware_concurrency(), 1, 16);
}
} // namespace

SoftwareRasterizer::SoftwareRasterizer(Gpu& gpu) : m_gpu(gpu), m_pool(std::make_unique<ThreadPool>(GpuThreads())) {}
SoftwareRasterizer::~SoftwareRasterizer() = default;

void SoftwareRasterizer::RasterizeAll(DrawState& st) {
    // Area total aproximada: con pocos pixeles no merece la pena despertar a los hilos
    double area = 0;
    for (const auto& t : st.tris)
        area += std::fabs(double(t.sv[1].x - t.sv[0].x) * (t.sv[2].y - t.sv[0].y) -
                          double(t.sv[2].x - t.sv[0].x) * (t.sv[1].y - t.sv[0].y)) * 0.5;
    const u32 n = area < 1024 ? 1 : m_pool->Size();
    // Constbufs del shader de pixeles: se cargan ya (los hilos solo los leen)
    for (u32 i = 0; i < 18; ++i) st.ReadConst(st.fs_group, i, 0);
    if (n > 1) {
        // Lo demas que se carga "la primera vez" tambien se carga ya, antes de repartir el trabajo
        for (const auto& t : st.targets) st.mm->Touch(t.surf.address, t.surf.SizeBytes());
        if (st.depth_enabled) st.mm->Touch(st.zsurf.address, st.zsurf.SizeBytes());
    }
    auto read = [mm = st.mm](u64 a, void* d, size_t sz) { mm->ReadBlock(a, d, sz); };
    std::vector<Worker> workers(n);
    for (u32 k = 0; k < n; ++k) {
        Worker& w = workers[k];
        w.interp.max_steps = st.interp.max_steps;
        w.fs = std::make_unique<ShaderProgram>(st.fs->Address(), read);
        if (k == 0) w.tex = st.textures.get();
        else {
            w.own_tex = st.textures->CloneEmpty();
            w.tex = w.own_tex.get();
        }
        w.rows.resize(st.targets.size());
    }
    m_pool->Run(n, [&](u32 k) {
        Worker& w = workers[k];
        for (const auto& t : st.tris) {
            RasterizeTriangle(st, w, t, k, n);
            if (w.failed) break;
        }
    });
    for (auto& w : workers) {
        st.pixels += w.pixels;
        m_gpu.GetStats().fs_instructions += w.fs_steps;
        if (w.failed && !st.failed) { st.failed = true; st.error = w.error; }
    }
}

void SoftwareRasterizer::Draw(const u32* regs, const ConstbufTable& cbs, const DrawCall& dc) {
    auto& stats = m_gpu.GetStats();
    DrawState st;
    st.regs = regs;
    st.cbs = &cbs;
    st.dc = &dc;
    st.mm = &m_gpu.MemoryManager();

    // --- Programas: vertices (etapa 1, "VertexB") y pixeles (etapa 5) ---
    const u64 region = Iova(regs[PROGRAM_REGION], regs[PROGRAM_REGION + 1]);
    auto stage_enabled = [&](u32 s) { return regs[PIPELINE + 0x10 * s] & 1; };
    for (u32 s : {0u, 2u, 3u, 4u})
        if (stage_enabled(s)) {
            m_gpu.Warn("stage" + std::to_string(s), "dibujo con teselado/geometria/VertexA: no soportado todavia");
            ++stats.draws_skipped;
            return;
        }
    if (!stage_enabled(1) || !stage_enabled(5)) {
        m_gpu.Warn("nostage", "dibujo sin shader de vertices o de pixeles");
        ++stats.draws_skipped;
        return;
    }
    auto read = [mm = st.mm](u64 a, void* d, size_t n) { mm->ReadBlock(a, d, n); };
    st.textures = std::make_unique<TextureSampler>(m_gpu, Iova(regs[TIC_POOL], regs[TIC_POOL + 1]), regs[TIC_MAX],
                                                   Iova(regs[TSC_POOL], regs[TSC_POOL + 1]), regs[TSC_MAX], &m_gpu.Textures());
    st.tex_cb = std::min<u32>(regs[BINDLESS_TEXTURE] & 0x1F, 17);
    st.vs = std::make_unique<ShaderProgram>(region + regs[PIPELINE + 0x10 * 1 + 1], read);
    st.fs = std::make_unique<ShaderProgram>(region + regs[PIPELINE + 0x10 * 5 + 1], read);
    st.vs_group = std::min<u32>(regs[PIPELINE + 0x10 * 1 + 4] & 7, 4);
    st.fs_group = std::min<u32>(regs[PIPELINE + 0x10 * 5 + 4] & 7, 4);
    if (st.vs_group == 0 && st.fs_group == 0) st.fs_group = 4;   // sin configurar: los habituales

    // --- Salidas del shader de pixeles (SPH): colores en R0.., profundidad detras ---
    const ShaderHeader& fh = st.fs->Header();
    {
        s32 reg = 0;
        for (u32 t = 0; t < 8; ++t)
            for (u32 c = 0; c < 4; ++c)
                st.out_reg[t][c] = ((fh.OmapTarget() >> (4 * t + c)) & 1) ? reg++ : -1;
        st.depth_reg = fh.OmapDepth() ? reg + 1 : -1;
    }
    st.mrt = ((fh.words[0] >> 14) & 1) && (regs[MRT_ENABLE] & 1);

    // --- Render targets ---
    const u32 ctl = regs[RT_CONTROL];
    const u32 num_targets = std::min<u32>(ctl & 0xF, 8);
    for (u32 k = 0; k < num_targets; ++k) {
        DrawState::Target t;
        t.index = (ctl >> (4 + 3 * k)) & 7;
        const u32 base = RT_BASE + t.index * 0x10;
        t.format = regs[base + 4];
        t.surf.address = Iova(regs[base], regs[base + 1]);
        t.surf.bytes_per_pixel = ColorFormatBytes(t.format);
        const u32 tile = regs[base + 5];
        t.surf.linear = (tile >> 12) & 1;
        if (t.surf.linear) {
            t.surf.pitch = regs[base + 2];
            t.surf.width = t.surf.bytes_per_pixel ? t.surf.pitch / t.surf.bytes_per_pixel : 0;
        } else {
            t.surf.width = regs[base + 2];
            t.surf.block_height_log2 = (tile >> 4) & 0xF;
        }
        t.surf.height = regs[base + 3];
        if (!t.surf.address || !t.surf.bytes_per_pixel || !t.surf.width || !t.surf.height) continue;
        const u32 wm = regs[CT_WRITE + ((regs[SINGLE_CT_WRITE] & 1) ? 0 : k)];
        t.write_mask = (wm & 1) | ((wm >> 3) & 2) | ((wm >> 6) & 4) | ((wm >> 9) & 8);
        t.blend = regs[BLEND_ENABLE + k] & 1;
        if (regs[BLEND_PER_TARGET] & 1) {
            for (u32 i = 0; i < 7; ++i) t.blend_regs[i] = regs[BLEND_TARGET + 8 * k + i];
        } else {
            const u32 g[7] = {regs[BLEND_GLOBAL], regs[BLEND_GLOBAL + 1], regs[BLEND_GLOBAL + 2], regs[BLEND_GLOBAL + 3],
                              regs[BLEND_GLOBAL + 4], regs[BLEND_GLOBAL + 5], regs[BLEND_GLOBAL + 7]};
            std::memcpy(t.blend_regs, g, sizeof(g));
        }
        float probe[4];
        u8 px[16] = {};
        if (t.blend && !DecodeColor(t.format, px, probe)) t.blend = false;   // formato entero: sin mezcla
        if (!EncodeColor(t.format, probe, px)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "dibujo: formato de color 0x%X no soportado", t.format);
            m_gpu.Warn("drawfmt" + std::to_string(t.format), buf);
            continue;
        }
        st.targets.push_back(t);
    }

    // --- Profundidad ---
    st.depth_test = regs[DEPTH_TEST] & 1;
    st.depth_write = regs[DEPTH_WRITE] & 1;
    st.depth_func = regs[DEPTH_FUNC];
    if ((regs[ZT_SELECT] & 1) && (st.depth_test || st.depth_write)) {
        st.zformat = regs[ZT_FORMAT];
        st.zsurf.address = Iova(regs[ZT_ADDR], regs[ZT_ADDR + 1]);
        st.zsurf.bytes_per_pixel = DepthFormatBytes(st.zformat);
        st.zsurf.width = regs[ZT_WIDTH];
        st.zsurf.height = regs[ZT_HEIGHT];
        st.zsurf.block_height_log2 = (regs[ZT_BLOCK] >> 4) & 0xF;
        float dummy;
        u8 zero[8] = {};
        st.depth_enabled = st.zsurf.address && st.zsurf.bytes_per_pixel && DecodeDepth(st.zformat, zero, dummy);
    }

    if (st.targets.empty() && !st.depth_enabled) { ++stats.draws_skipped; return; }

    // --- Zona donde se puede dibujar: render target, viewport, scissors ---
    u32 w = 0xFFFF, h = 0xFFFF;
    for (const auto& t : st.targets) { w = std::min(w, t.surf.width); h = std::min(h, t.surf.height); }
    if (st.depth_enabled) { w = std::min(w, st.zsurf.width); h = std::min(h, st.zsurf.height); }
    MsaaSampleGrid(regs[MULTISAMPLE_MODE], st.msx, st.msy);   // las superficies miden en muestras
    w /= st.msx;
    h /= st.msy;
    st.bx0 = 0; st.by0 = 0; st.bx1 = w; st.by1 = h;
    auto clip_rect = [&](u32 x0, u32 x1, u32 y0, u32 y1) {
        st.bx0 = std::max(st.bx0, x0); st.bx1 = std::min(st.bx1, x1);
        st.by0 = std::max(st.by0, y0); st.by1 = std::min(st.by1, y1);
    };
    const u32 vh = regs[VIEWPORT], vv = regs[VIEWPORT + 1];
    if (vh >> 16 && vv >> 16) clip_rect(vh & 0xFFFF, (vh & 0xFFFF) + (vh >> 16), vv & 0xFFFF, (vv & 0xFFFF) + (vv >> 16));
    const u32 sh = regs[SCREEN_SCISSOR_H], sv = regs[SCREEN_SCISSOR_V];
    if (sh >> 16 && sv >> 16) clip_rect(sh & 0xFFFF, (sh & 0xFFFF) + (sh >> 16), sv & 0xFFFF, (sv & 0xFFFF) + (sv >> 16));
    if (regs[SCISSOR] & 1) {
        const u32 h0 = regs[SCISSOR + 1], v0 = regs[SCISSOR + 2];
        clip_rect(h0 & 0xFFFF, h0 >> 16, v0 & 0xFFFF, v0 >> 16);
    }
    if (st.bx0 >= st.bx1 || st.by0 >= st.by1) return;
    st.flip_y = (regs[WINDOW_ORIGIN] >> 4) & 1;
    st.y_direction = (regs[WINDOW_ORIGIN] & 1) ? -1.0f : 1.0f;

    // --- Vertices: ejecutar el shader de vertices en cada uno (con cache por indice) ---
    const bool restart = regs[PRIM_RESTART] & 1;
    const u32 restart_index = regs[PRIM_RESTART_INDEX];
    const u32 index_size = 1u << std::min<u32>(regs[INDEX_FORMAT] & 3, 2);
    const u64 index_addr = Iova(regs[INDEX_ADDR], regs[INDEX_ADDR + 1]);
    std::vector<s64> indices;       // -1 = reinicio de primitiva
    indices.reserve(dc.count);
    for (u32 i = 0; i < dc.count; ++i) {
        if (dc.indexed) {
            u32 idx = 0;
            st.mm->ReadBlock(index_addr + u64(dc.first + i) * index_size, &idx, index_size);
            if (restart && idx == restart_index) { indices.push_back(-1); continue; }
            indices.push_back(s64(s32(idx) + dc.vertex_offset) & 0xFFFFFFFF);
        } else {
            indices.push_back(s64(dc.first) + i);
        }
    }
    std::map<u32, size_t> cache;
    std::vector<Vertex> verts;
    std::vector<s64> slots;         // posicion en 'verts' de cada indice (-1 = reinicio)
    slots.reserve(indices.size());
    for (s64 idx : indices) {
        if (idx < 0) { slots.push_back(-1); continue; }
        auto it = cache.find(u32(idx));
        if (it != cache.end()) { slots.push_back(s64(it->second)); continue; }
        Vertex v;
        const u32 vid = dc.indexed ? u32(idx) : u32(idx - dc.first) + dc.vertex_id_base;
        if (!RunVertexShader(st, u32(idx), vid, v)) break;
        cache[u32(idx)] = verts.size();
        slots.push_back(s64(verts.size()));
        verts.push_back(v);
    }
    if (st.failed) {
        m_gpu.Warn("vs:" + st.error, st.error);
        ++stats.shader_errors;
        return;
    }

    // --- Primitivas ---
    const bool last = regs[PROVOKING_LAST] & 1;
    auto tri = [&](s64 a, s64 b, s64 c, s64 prov) {
        if (a < 0 || b < 0 || c < 0 || st.failed) return;
        ProcessTriangle(st, verts[a], verts[b], verts[c], verts[prov]);
    };
    // Recorre la lista en trozos separados por los reinicios de primitiva
    size_t start = 0;
    while (start < slots.size() && !st.failed) {
        size_t end = start;
        while (end < slots.size() && slots[end] >= 0) ++end;
        const s64* s = slots.data() + start;
        const size_t n = end - start;
        switch (dc.topology) {
            case 4:   // triangulos
                for (size_t i = 0; i + 2 < n; i += 3) tri(s[i], s[i + 1], s[i + 2], last ? s[i + 2] : s[i]);
                break;
            case 5:   // tira: cada triangulo nuevo alterna el sentido para que todos giren igual
                for (size_t i = 0; i + 2 < n; ++i) {
                    if (i & 1) tri(s[i + 1], s[i], s[i + 2], last ? s[i + 2] : s[i]);
                    else tri(s[i], s[i + 1], s[i + 2], last ? s[i + 2] : s[i]);
                }
                break;
            case 6: case 9:   // abanico y poligono
                for (size_t i = 1; i + 1 < n; ++i) tri(s[0], s[i], s[i + 1], dc.topology == 9 ? s[0] : (last ? s[i + 1] : s[0]));
                break;
            case 7:   // quads: dos triangulos cada uno
                for (size_t i = 0; i + 3 < n; i += 4) {
                    tri(s[i], s[i + 1], s[i + 2], last ? s[i + 3] : s[i]);
                    tri(s[i], s[i + 2], s[i + 3], last ? s[i + 3] : s[i]);
                }
                break;
            case 8:   // tira de quads
                for (size_t i = 0; i + 3 < n; i += 2) {
                    tri(s[i], s[i + 1], s[i + 3], last ? s[i + 3] : s[i]);
                    tri(s[i], s[i + 3], s[i + 2], last ? s[i + 3] : s[i]);
                }
                break;
            default: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "dibujo: primitiva %u no soportada (puntos/lineas)", dc.topology);
                m_gpu.Warn("topo" + std::to_string(dc.topology), buf);
                start = slots.size();
                continue;
            }
        }
        start = end + 1;
    }
    if (!st.failed && !st.tris.empty()) {
        RasterizeAll(st);
        // Lo dibujado puede ser una textura que la cache tiene decodificada
        for (const auto& t : st.targets) st.mm->NotifyWrite(t.surf.address, t.surf.SizeBytes());
        if (st.depth_enabled && st.depth_write) st.mm->NotifyWrite(st.zsurf.address, st.zsurf.SizeBytes());
    }
    if (st.failed) {
        m_gpu.Warn("fs:" + st.error, st.error);
        ++stats.shader_errors;
    }
    if (static const bool dbg = std::getenv("NEXO2_DRAW_DEBUG") != nullptr; dbg)
        std::fprintf(stderr, "draw %llu: tris %llu pix %llu fs %llx rt %u fmt %X blend %d tex %d box %u,%u-%u,%u\n", (unsigned long long)stats.draws,
            (unsigned long long)st.triangles, (unsigned long long)st.pixels, (unsigned long long)(st.fs ? st.fs->Address() : 0),
            st.targets.empty() ? 0 : st.targets[0].surf.width, st.targets.empty() ? 0 : st.targets[0].format,
            st.targets.empty() ? 0 : int(st.targets[0].blend), 0, st.bx0, st.by0, st.bx1, st.by1);
    ++stats.draws;
    stats.triangles += st.triangles;
    stats.pixels += st.pixels;
}

// ============================================================================
//  Recorte y paso a pantalla
// ============================================================================

void SoftwareRasterizer::ProcessTriangle(DrawState& st, const Vertex& a, const Vertex& b, const Vertex& c,
                                         const Vertex& provoking) {
    // Planos de recorte en coordenadas de recorte: dentro si dist >= 0.
    // Cercano (z >= -w o z >= 0), lejano (z <= w), w > 0 y una banda de guarda en x/y
    // (8 veces el viewport) para que las coordenadas de pantalla no se salgan de rango.
    const bool zero_to_one = st.regs[DEPTH_MODE] & 1;
    constexpr float GUARD = 8.0f;
    auto dist = [&](const Vertex& v, int plane) {
        const float x = v.Pos(0), y = v.Pos(1), z = v.Pos(2), w = v.Pos(3);
        switch (plane) {
            case 0: return zero_to_one ? z : z + w;
            case 1: return w - z;
            case 2: return w - 1e-6f;
            case 3: return GUARD * w - x;
            case 4: return GUARD * w + x;
            case 5: return GUARD * w - y;
            default: return GUARD * w + y;
        }
    };
    std::vector<const Vertex*> poly = {&a, &b, &c};
    for (int plane = 0; plane < 7 && poly.size() >= 3; ++plane) {
        bool all_in = true;
        for (const Vertex* v : poly) if (dist(*v, plane) < 0) { all_in = false; break; }
        if (all_in) continue;
        std::vector<const Vertex*> out;
        for (size_t i = 0; i < poly.size(); ++i) {
            const Vertex* p = poly[i];
            const Vertex* q = poly[(i + 1) % poly.size()];
            const float dp = dist(*p, plane), dq = dist(*q, plane);
            if (dp >= 0) out.push_back(p);
            if ((dp >= 0) != (dq >= 0)) {
                const float t = dp / (dp - dq);
                Vertex& n = st.scratch.emplace_back(*p);
                // Interpolar posicion y atributos (a[0x70]..a[0x27F]) en coordenadas de recorte
                for (u32 k = 0x70 / 4; k < 0x280 / 4; ++k)
                    n.attr[k] = U(F(p->attr[k]) + (F(q->attr[k]) - F(p->attr[k])) * t);
                out.push_back(&n);
            }
        }
        poly.swap(out);
    }
    if (poly.size() < 3) return;

    // A pantalla: division por w y viewport 0
    const u32* r = st.regs;
    const bool xform = r[VIEWPORT_XFORM_ENABLE] & 1;
    const float sx = F(r[VIEWPORT_XFORM]), sy = F(r[VIEWPORT_XFORM + 1]), sz = F(r[VIEWPORT_XFORM + 2]);
    const float ox = F(r[VIEWPORT_XFORM + 3]), oy = F(r[VIEWPORT_XFORM + 4]), oz = F(r[VIEWPORT_XFORM + 5]);
    std::vector<ScreenVertex> sv(poly.size());
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vertex& v = *poly[i];
        const float iw = 1.0f / v.Pos(3);
        const float nx = v.Pos(0) * iw, ny = v.Pos(1) * iw, nz = v.Pos(2) * iw;
        sv[i].v = &v;
        sv[i].inv_w = iw;
        if (xform) { sv[i].x = sx * nx + ox; sv[i].y = sy * ny + oy; sv[i].z = sz * nz + oz; }
        else { sv[i].x = nx; sv[i].y = ny; sv[i].z = nz; }
    }

    // Cara delantera o trasera: el sentido de giro en pantalla (con y hacia abajo,
    // "contrario a las agujas del reloj" da area negativa salvo que FlipY lo invierta)
    float area = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
        const auto& p = sv[i]; const auto& q = sv[(i + 1) % sv.size()];
        area += p.x * q.y - q.x * p.y;
    }
    if (area == 0) return;
    const bool ccw = st.flip_y ? area > 0 : area < 0;
    const bool front = ccw == (r[FRONT_FACE] == 0x901);
    if (r[CULL_ENABLE] & 1) {
        const u32 cull = r[CULL_FACE];
        if (cull == 0x408 || (cull == 0x404 && front) || (cull == 0x405 && !front)) return;
    }
    for (size_t i = 1; i + 1 < sv.size(); ++i) st.tris.push_back({{sv[0], sv[i], sv[i + 1]}, &provoking, front});
    ++st.triangles;
}

// ============================================================================
//  Pixeles
// ============================================================================

namespace {
class PixelEnv final : public ShaderEnv {
public:
    std::function<u32(u32, u32)> read_const;
    const SoftwareRasterizer::Vertex* v[3] = {};
    const SoftwareRasterizer::Vertex* provoking = nullptr;
    float inv_w[3] = {};
    float l[3] = {};            // coordenadas baricentricas del pixel
    float frag[4] = {};         // gl_FragCoord: x, y, z, 1/w
    bool front = true;
    float y_direction = 1.0f;

    u32 ReadConst(u32 index, u32 offset) override { return read_const(index, offset); }
    u32 ReadAttribute(u32 addr) override {
        varying = true;
        if (addr == 0x3FC) return front ? 0xFFFFFFFFu : 0;   // gl_FrontFacing
        if (addr >= 0x70 && addr < 0x80) return U(frag[(addr - 0x70) / 4]);
        return 0;
    }
    void WriteAttribute(u32, u32) override {}
    float Interpolate(u32 addr, u32 mode) override {
        varying = true;
        if (addr >= 0x70 && addr < 0x80) return frag[(addr - 0x70) / 4];
        if (addr == 0x3FC) return front ? 1.0f : 0.0f;
        if (addr >= 0x400) return 0.0f;
        const u32 k = addr / 4;
        if (mode == 2) return F(provoking->attr[k]);   // constante (flat): del vertice que manda
        const float a0 = F(v[0]->attr[k]), a1 = F(v[1]->attr[k]), a2 = F(v[2]->attr[k]);
        if (mode == 0) return l[0] * a0 + l[1] * a1 + l[2] * a2;   // lineal en pantalla
        // Perspectiva: se interpola atributo / w; el shader multiplica luego por w
        return l[0] * a0 * inv_w[0] + l[1] * a1 * inv_w[1] + l[2] * a2 * inv_w[2];
    }
    u32 SystemRegister(u32 sr) override {
        if (sr != 0x12) varying = true;
        if (sr == 0x12) return U(y_direction);   // SR_Y_DIRECTION
        return 0;
    }
    void SampleTexture(u32 handle, const TextureRequest& req, u32 out[4]) override { varying = true; tex->Sample(handle, req, out); }
    void QueryTexture(u32 handle, u32 lod, u32 out[4]) override { varying = true; tex->Query(handle, lod, out); }
    // El shader leyo algo que cambia de un pixel a otro (atributos, texturas...). Si no, su
    // resultado es el mismo en todo el triangulo y basta con ejecutarlo una vez.
    bool varying = false;
    u32 TextureConstbuf() override { return tex_cb; }
    TextureSampler* tex = nullptr;
    u32 tex_cb = 0;
};
} // namespace

void SoftwareRasterizer::RasterizeTriangle(DrawState& st, Worker& w, const RasterTri& tri, u32 band, u32 bands) {
    const ScreenVertex* sv = tri.sv;
    // Coordenadas en punto fijo (1/256 de pixel): las aristas compartidas entre dos
    // triangulos dan exactamente los mismos valores, sin huecos ni pixeles repetidos.
    s64 X[3], Y[3];
    int o[3] = {0, 1, 2};
    for (int i = 0; i < 3; ++i) { X[i] = std::llround(double(sv[i].x) * 256.0); Y[i] = std::llround(double(sv[i].y) * 256.0); }
    s64 area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0) return;
    if (area < 0) { std::swap(o[1], o[2]); area = -area; }
    const s64 x0 = X[o[0]], y0 = Y[o[0]], x1 = X[o[1]], y1 = Y[o[1]], x2 = X[o[2]], y2 = Y[o[2]];

    // Caja que contiene el triangulo, recortada a la zona dibujable
    auto floor_px = [](s64 v) { return s64(std::floor(double(v) / 256.0)); };
    s64 minx = std::max<s64>(floor_px(std::min({x0, x1, x2})), st.bx0);
    s64 maxx = std::min<s64>(floor_px(std::max({x0, x1, x2})) + 1, st.bx1);
    s64 miny = std::max<s64>(floor_px(std::min({y0, y1, y2})), st.by0);
    s64 maxy = std::min<s64>(floor_px(std::max({y0, y1, y2})) + 1, st.by1);
    if (minx >= maxx || miny >= maxy) return;

    // Arista a->b: E(P) = dx * (Py - ay) - dy * (Px - ax), positivo dentro.
    // Regla "arriba-izquierda": un pixel justo en el borde solo cuenta si la arista es
    // de arriba o de la izquierda (asi dos triangulos vecinos no pintan dos veces).
    struct Edge { s64 ax, ay, dx, dy; s64 bias; };
    auto make_edge = [](s64 ax, s64 ay, s64 bx, s64 by) {
        Edge e{ax, ay, bx - ax, by - ay, 0};
        const bool top_left = e.dy < 0 || (e.dy == 0 && e.dx > 0);
        e.bias = top_left ? 0 : -1;
        return e;
    };
    const Edge e12 = make_edge(x1, y1, x2, y2), e20 = make_edge(x2, y2, x0, y0), e01 = make_edge(x0, y0, x1, y1);
    auto eval = [](const Edge& e, s64 px, s64 py) { return e.dx * (py - e.ay) - e.dy * (px - e.ax); };

    PixelEnv env;
    env.read_const = [&st](u32 i, u32 o2) { return st.ReadConst(st.fs_group, i, o2); };
    env.cb_direct = st.fs_group < 5;
    if (env.cb_direct)
        for (u32 i = 0; i < 18; ++i) {
            env.cb_data[i] = st.cb_cache[st.fs_group][i].data();
            env.cb_words[i] = u32(st.cb_cache[st.fs_group][i].size());
        }
    for (int i = 0; i < 3; ++i) { env.v[i] = sv[o[i]].v; env.inv_w[i] = sv[o[i]].inv_w; }
    env.provoking = tri.provoking;
    env.tex = w.tex;
    env.tex_cb = st.tex_cb;
    env.front = tri.front;
    env.y_direction = st.y_direction;
    const float z0 = sv[o[0]].z, z1 = sv[o[1]].z, z2 = sv[o[2]].z;
    const double inv_area = 1.0 / double(area);
    const u32 span = u32(maxx - minx);
    const float bconst[4] = {F(st.regs[BLEND_CONST]), F(st.regs[BLEND_CONST + 1]), F(st.regs[BLEND_CONST + 2]),
                             F(st.regs[BLEND_CONST + 3])};

    auto& rows = w.rows;
    auto& zrow = w.zrow;
    bool uniform = false;                         // el shader da lo mismo en todo el triangulo
    bool uniform_enc_ready = false;
    std::array<std::array<u8, 16>, 8> uniform_enc{};
    std::array<bool, 8> uniform_enc_ok{};
    // Si cada pixel de la fila se va a escribir entero (sin mezcla, sin mascara, sin
    // profundidad y sin pixeles descartados), no hace falta leer antes la fila
    bool overwrite = !st.depth_enabled && !w.fs->Header().KillsPixels();
    for (const auto& tg : st.targets)
        if (tg.blend || ColorWriteByteMask(tg.format, tg.write_mask) != (1u << tg.surf.bytes_per_pixel) - 1) overwrite = false;
    for (s64 py = miny; py < maxy && !w.failed; ++py) {
        if (bands > 1 && u32(py >> 2) % bands != band) continue;   // fila de otro hilo
        const s64 cy = py * 256 + 128;
        // Que pixeles de esta fila estan dentro (para no leer/escribir filas vacias).
        // Las aristas cambian lo mismo en cada pixel: se suman en vez de recalcularlas.
        s64 first = -1, lastx = -1;
        {
            const s64 cx0 = minx * 256 + 128;
            s64 a = eval(e12, cx0, cy) + e12.bias, b = eval(e20, cx0, cy) + e20.bias, c = eval(e01, cx0, cy) + e01.bias;
            const s64 sa = -e12.dy * 256, sb = -e20.dy * 256, sc = -e01.dy * 256;
            for (s64 px = minx; px < maxx; ++px, a += sa, b += sb, c += sc) {
                if ((a | b | c) >= 0) {
                    if (first < 0) first = px;
                    lastx = px;
                } else if (first >= 0) {
                    break;   // un triangulo es convexo: despues del tramo de dentro ya no hay mas
                }
            }
        }
        if (first < 0) continue;
        const u32 fx = u32(first), n = u32(lastx - first + 1);
        // Camino rapido: color fijo que tapa la fila entera -> rellenar y escribir
        if (uniform && overwrite && uniform_enc_ready) {
            bool all = true;
            for (size_t t = 0; t < st.targets.size(); ++t) all = all && t < uniform_enc_ok.size() && uniform_enc_ok[t];
            if (all) {
                for (size_t t = 0; t < st.targets.size(); ++t) {
                    const u32 bpp = st.targets[t].surf.bytes_per_pixel;
                    rows[t].resize(size_t(n) * bpp);
                    for (u32 i = 0; i < n; ++i) std::memcpy(&rows[t][size_t(i) * bpp], uniform_enc[t].data(), bpp);
                    WritePixels(*st.mm, st.targets[t].surf, st.msx, st.msy, fx, u32(py), n, rows[t].data(), w.ms_tmp);
                }
                w.pixels += n;
                continue;
            }
        }
        for (size_t t = 0; t < st.targets.size(); ++t) {
            rows[t].resize(size_t(n) * st.targets[t].surf.bytes_per_pixel);
            if (!overwrite) ReadPixels(*st.mm, st.targets[t].surf, st.msx, st.msy, fx, u32(py), n, rows[t].data(), w.ms_tmp);
        }
        if (st.depth_enabled) {
            zrow.resize(size_t(n) * st.zsurf.bytes_per_pixel);
            ReadPixels(*st.mm, st.zsurf, st.msx, st.msy, fx, u32(py), n, zrow.data(), w.ms_tmp);
        }
        bool zdirty = false;
        const s64 fcx = first * 256 + 128;
        s64 w0 = eval(e12, fcx, cy) + e12.dy * 256, w1 = eval(e20, fcx, cy) + e20.dy * 256, w2 = eval(e01, fcx, cy) + e01.dy * 256;
        for (s64 px = first; px <= lastx; ++px) {
            w0 -= e12.dy * 256; w1 -= e20.dy * 256; w2 -= e01.dy * 256;
            if (w0 + e12.bias < 0 || w1 + e20.bias < 0 || w2 + e01.bias < 0) continue;
            env.l[0] = float(double(w0) * inv_area);
            env.l[1] = float(double(w1) * inv_area);
            env.l[2] = float(double(w2) * inv_area);
            float z = env.l[0] * z0 + env.l[1] * z1 + env.l[2] * z2;
            env.frag[0] = float(px) + 0.5f;
            env.frag[1] = float(py) + 0.5f;
            env.frag[2] = z;
            env.frag[3] = env.l[0] * env.inv_w[0] + env.l[1] * env.inv_w[1] + env.l[2] * env.inv_w[2];
            const u32 i = u32(px - first);
            u8* zpx = st.depth_enabled ? &zrow[size_t(i) * st.zsurf.bytes_per_pixel] : nullptr;
            float stored = 0;
            const bool early = st.depth_enabled && st.depth_test && st.depth_reg < 0;
            if (early) {
                DecodeDepth(st.zformat, zpx, stored);
                if (!CompareFunc(st.depth_func, std::clamp(z, 0.0f, 1.0f), stored)) continue;
            }
            if (!uniform) {
                env.varying = false;
                const auto res = w.interp.Run(*w.fs, env);
                w.fs_steps += w.interp.steps;
                if (!res.ok) { w.failed = true; w.error = "shader de pixeles: " + res.error; break; }
                if (res.killed) {
                    if (overwrite)   // la fila no se leyo: este pixel tiene que quedar como estaba
                        for (size_t t = 0; t < st.targets.size(); ++t)
                            ReadPixels(*st.mm, st.targets[t].surf, st.msx, st.msy, u32(px), u32(py), 1,
                                       &rows[t][size_t(px - first) * st.targets[t].surf.bytes_per_pixel], w.ms_tmp);
                    continue;
                }
                // Color fijo (por ejemplo un rectangulo de un solo color): los registros de
                // salida ya tienen el resultado para todos los pixeles que quedan
                if (!env.varying) { uniform = true; uniform_enc_ready = false; }
            }
            ++w.pixels;
            if (st.depth_reg >= 0) z = w.interp.RegF(u32(st.depth_reg));
            z = std::clamp(z, 0.0f, 1.0f);
            if (st.depth_enabled) {
                if (st.depth_test && !early) {
                    DecodeDepth(st.zformat, zpx, stored);
                    if (!CompareFunc(st.depth_func, z, stored)) continue;
                }
                if (st.depth_write) { EncodeDepthStencil(st.zformat, z, 0, true, false, zpx); zdirty = true; }
            }
            for (size_t t = 0; t < st.targets.size(); ++t) {
                const auto& tg = st.targets[t];
                const u32 out = st.mrt ? u32(t) : 0;
                float src[4];
                for (u32 c = 0; c < 4; ++c) {
                    const s32 reg = st.out_reg[out][c];
                    src[c] = reg >= 0 ? w.interp.RegF(u32(reg)) : (c == 3 ? 1.0f : 0.0f);
                }
                u8* dst_px = &rows[t][size_t(i) * tg.surf.bytes_per_pixel];
                if (tg.blend) {
                    float dst[4];
                    DecodeColor(tg.format, dst_px, dst);
                    const u32* b = tg.blend_regs;
                    const bool separate = b[0] & 1;
                    float res2[4];
                    for (int c = 0; c < 4; ++c) {
                        const bool alpha = c == 3 && separate;
                        const u32 op = alpha ? b[4] : b[1], fsr = alpha ? b[5] : b[2], fds = alpha ? b[6] : b[3];
                        res2[c] = BlendEquation(op, src[c], dst[c], BlendFactor(fsr, src, dst, bconst, c),
                                                BlendFactor(fds, src, dst, bconst, c));
                    }
                    std::memcpy(src, res2, sizeof(src));
                }
                u8 enc_buf[16];
                const u8* enc = enc_buf;
                if (uniform && !tg.blend && t < uniform_enc.size()) {
                    if (!uniform_enc_ready) {
                        for (size_t k = 0; k < st.targets.size() && k < uniform_enc.size(); ++k) uniform_enc_ok[k] = false;
                        uniform_enc_ready = true;
                    }
                    if (!uniform_enc_ok[t]) {
                        uniform_enc_ok[t] = EncodeColor(tg.format, src, uniform_enc[t].data());
                        if (!uniform_enc_ok[t]) continue;
                    }
                    enc = uniform_enc[t].data();
                } else if (!EncodeColor(tg.format, src, enc_buf)) {
                    continue;
                }
                const u32 bytes_mask = ColorWriteByteMask(tg.format, tg.write_mask);
                for (u32 bi = 0; bi < tg.surf.bytes_per_pixel; ++bi)
                    if (bytes_mask & (1u << bi)) dst_px[bi] = enc[bi];
            }
        }
        for (size_t t = 0; t < st.targets.size(); ++t)
            WritePixels(*st.mm, st.targets[t].surf, st.msx, st.msy, fx, u32(py), n, rows[t].data(), w.ms_tmp);
        if (zdirty) WritePixels(*st.mm, st.zsurf, st.msx, st.msy, fx, u32(py), n, zrow.data(), w.ms_tmp);
        (void)span;
    }
}

} // namespace NeXo2::GPU
