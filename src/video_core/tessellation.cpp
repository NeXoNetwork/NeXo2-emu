#include "tessellation.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include "shader.hpp"
#include "texture.hpp"

namespace NeXo2::GPU {

namespace {
inline float F(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
inline u32 U(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }
inline u64 Iova(u32 hi, u32 lo) { return (u64(hi & 0xFF) << 32) | lo; }

constexpr u32 TESS_MODE = 0x0C8, TESS_OUTER = 0x0C9, TESS_INNER = 0x0CD, PATCH_SIZE = 0x373;
constexpr u32 PROGRAM_REGION = 0x582, PIPELINE = 0x800;

using Vertex = SoftwareRasterizer::Vertex;

// Nivel de teselado -> numero de segmentos segun el espaciado
int RoundLevel(float x, u32 spacing) {
    if (std::isnan(x)) x = 1;
    int n;
    if (spacing == 1) {          // fraccional impar: [1, 63], hacia arriba al impar
        n = int(std::ceil(std::clamp(x, 1.0f, 63.0f)));
        if (!(n & 1)) ++n;
    } else if (spacing == 2) {   // fraccional par: [2, 64], hacia arriba al par
        n = int(std::ceil(std::clamp(x, 2.0f, 64.0f)));
        if (n & 1) ++n;
    } else {                     // igual: [1, 64], hacia arriba
        n = int(std::ceil(std::clamp(x, 1.0f, 64.0f)));
    }
    return n;
}
// Nivel interior de 1 cuando el parche no es "todo unos": cuenta como 1 + epsilon
int Bump(int n, u32 spacing) { return n > 1 ? n : spacing == 1 ? 3 : 2; }

class Builder {
public:
    Builder(TessDomain& d, bool cw) : m_d(d), m_cw(cw) {}
    u32 Point(float u, float v) { m_d.points.push_back({u, v}); return u32(m_d.points.size() - 1); }
    // Triangulo con el sentido pedido (en el plano u, v); los de area 0 se descartan
    void Tri(u32 a, u32 b, u32 c) {
        const auto& p = m_d.points;
        const float area = (p[b][0] - p[a][0]) * (p[c][1] - p[a][1]) - (p[c][0] - p[a][0]) * (p[b][1] - p[a][1]);
        if (std::fabs(area) < 1e-12f) return;
        if ((area < 0) != m_cw) std::swap(b, c);
        m_d.tris.push_back({a, b, c});
    }
    // Polilinea de 'segs' tramos entre dos puntos ya creados
    std::vector<u32> Edge(u32 a, u32 b, int segs) {
        std::vector<u32> e{a};
        const auto pa = m_d.points[a], pb = m_d.points[b];
        for (int i = 1; i < segs; ++i) {
            const float t = float(i) / float(segs);
            e.push_back(Point(pa[0] + (pb[0] - pa[0]) * t, pa[1] + (pb[1] - pa[1]) * t));
        }
        if (segs > 0) e.push_back(b);
        return e;
    }
    // Une dos polilineas paralelas (exterior e interior) con triangulos
    void Stitch(const std::vector<u32>& o, const std::vector<u32>& in) {
        const size_t mo = o.size() - 1, mi = in.size() - 1;
        size_t i = 0, j = 0;
        while (i < mo || j < mi) {
            bool outer;
            if (i == mo) outer = false;
            else if (j == mi) outer = true;
            else outer = (double(i) + 0.5) / double(mo) <= (double(j) + 0.5) / double(mi);
            if (outer) { Tri(o[i], o[i + 1], in[j]); ++i; }
            else { Tri(o[i], in[j + 1], in[j]); ++j; }
        }
    }
    // Rejilla de su x sv cuadros entre (u0, v0) y (u1, v1)
    void Grid(float u0, float v0, float u1, float v1, int su, int sv) {
        std::vector<u32> idx;
        for (int y = 0; y <= sv; ++y)
            for (int x = 0; x <= su; ++x)
                idx.push_back(Point(u0 + (u1 - u0) * float(x) / float(su), v0 + (v1 - v0) * float(y) / float(sv)));
        auto at = [&](int x, int y) { return idx[size_t(y) * size_t(su + 1) + size_t(x)]; };
        for (int y = 0; y < sv; ++y)
            for (int x = 0; x < su; ++x) {
                Tri(at(x, y), at(x + 1, y), at(x + 1, y + 1));
                Tri(at(x, y), at(x + 1, y + 1), at(x, y + 1));
            }
    }

private:
    TessDomain& m_d;
    bool m_cw;
};

void TessTriangle(Builder& b, u32 spacing, const float outer[4], const float inner[2]) {
    int o[3] = {RoundLevel(outer[0], spacing), RoundLevel(outer[1], spacing), RoundLevel(outer[2], spacing)};
    int n = RoundLevel(inner[0], spacing);
    // Esquinas: u = 1, v = 1 y w = 1 (en el plano u, v: (1,0), (0,1) y (0,0))
    const float V[3][2] = {{1, 0}, {0, 1}, {0, 0}};
    if (n == 1 && o[0] == 1 && o[1] == 1 && o[2] == 1) {
        b.Tri(b.Point(1, 0), b.Point(0, 1), b.Point(0, 0));
        return;
    }
    n = Bump(n, spacing);
    // Anillo k: triangulo con el mismo centro, lados (n - 2k) / n del exterior. Lados: 0 = u = 0
    // (de la esquina 1 a la 2, nivel exterior 0), 1 = v = 0 (2 -> 0), 2 = w = 0 (0 -> 1).
    const float C = 1.0f / 3.0f;
    auto ring = [&](int k) {
        std::array<std::vector<u32>, 3> edges;
        const int m = n - 2 * k;
        const float s = float(m) / float(n);
        if (m <= 0) {
            const u32 c = b.Point(C, C);
            for (auto& e : edges) e = {c};
            return edges;
        }
        u32 c[3];
        for (int i = 0; i < 3; ++i) c[i] = b.Point(C + s * (V[i][0] - C), C + s * (V[i][1] - C));
        const int from[3] = {1, 2, 0}, to[3] = {2, 0, 1};
        for (int e = 0; e < 3; ++e) edges[e] = b.Edge(c[from[e]], c[to[e]], k == 0 ? o[e] : m);
        return edges;
    };
    auto prev = ring(0);
    int k = 0;
    while (n - 2 * (k + 1) >= 0) {
        auto next = ring(k + 1);
        for (int e = 0; e < 3; ++e) b.Stitch(prev[e], next[e]);
        prev = std::move(next);
        ++k;
    }
    if (n - 2 * k == 1) b.Tri(prev[2][0], prev[0][0], prev[1][0]);   // el triangulo del centro
}

void TessQuad(Builder& b, u32 spacing, const float outer[4], const float inner[2]) {
    int o[4];
    for (int i = 0; i < 4; ++i) o[i] = RoundLevel(outer[i], spacing);
    int n0 = RoundLevel(inner[0], spacing), n1 = RoundLevel(inner[1], spacing);
    if (n0 == 1 && n1 == 1 && o[0] == 1 && o[1] == 1 && o[2] == 1 && o[3] == 1) {
        b.Grid(0, 0, 1, 1, 1, 1);
        return;
    }
    n0 = Bump(n0, spacing);
    n1 = Bump(n1, spacing);
    // Anillo k: rectangulo (k/n0, k/n1) - (1-k/n0, 1-k/n1). Lados: abajo (v = v0, nivel 1),
    // derecha (u = u1, nivel 2), arriba (v = v1, nivel 3), izquierda (u = u0, nivel 0)
    auto ring = [&](int k) {
        const float u0 = float(k) / float(n0), u1 = 1 - u0, v0 = float(k) / float(n1), v1 = 1 - v0;
        const int su = n0 - 2 * k, sv = n1 - 2 * k;
        const u32 c[4] = {b.Point(u0, v0), b.Point(u1, v0), b.Point(u1, v1), b.Point(u0, v1)};
        std::array<std::vector<u32>, 4> edges;
        edges[0] = b.Edge(c[0], c[1], k == 0 ? o[1] : su);
        edges[1] = b.Edge(c[1], c[2], k == 0 ? o[2] : sv);
        edges[2] = b.Edge(c[2], c[3], k == 0 ? o[3] : su);
        edges[3] = b.Edge(c[3], c[0], k == 0 ? o[0] : sv);
        for (auto& e : edges) if (e.size() == 1) e.push_back(e[0]);   // lado de 0 tramos: un punto
        return edges;
    };
    const int K = std::min(n0, n1) / 2;
    auto prev = ring(0);
    for (int k = 0; k < K; ++k) {
        auto next = ring(k + 1);
        for (int e = 0; e < 4; ++e) {
            // Un lado interior sin tramos es un punto: se une como polilinea de un punto
            std::vector<u32> in = next[e];
            if (in.size() == 2 && in[0] == in[1]) in.pop_back();
            b.Stitch(prev[e], in);
        }
        prev = std::move(next);
    }
    const int su = n0 - 2 * K, sv = n1 - 2 * K;
    if (su > 0 && sv > 0) {   // queda un rectangulo de verdad en el centro
        const float u0 = float(K) / float(n0), v0 = float(K) / float(n1);
        b.Grid(u0, v0, 1 - u0, 1 - v0, su, sv);
    }
}

void TessIsolines(TessDomain& d, u32 spacing, const float outer[4]) {
    const int count = RoundLevel(outer[0], 0);
    const int segs = RoundLevel(outer[1], spacing);
    for (int i = 0; i < count; ++i) {
        const float v = float(i) / float(count);
        const u32 base = u32(d.points.size());
        for (int j = 0; j <= segs; ++j) d.points.push_back({float(j) / float(segs), v});
        for (int j = 0; j < segs; ++j) d.lines.push_back({base + u32(j), base + u32(j) + 1});
    }
}

// Constbufs de un grupo de etapas, copiados la primera vez que se leen
struct ConstReader {
    ConstReader(GpuMemoryManager& mm, const ConstbufTable& cbs, u32 group) : mm(mm), cbs(cbs), group(group) {}
    u32 Read(u32 index, u32 offset) {
        if (index >= 18) return 0;
        if (!loaded[index]) {
            loaded[index] = true;
            const ConstbufBinding& b = cbs[group][index];
            if (b.valid && b.size) {
                data[index].resize((std::min<u32>(b.size, 0x10000) + 3) / 4);
                mm.ReadBlock(b.address, data[index].data(), data[index].size() * 4);
            }
        }
        return offset / 4 < data[index].size() ? data[index][offset / 4] : 0;
    }
    GpuMemoryManager& mm;
    const ConstbufTable& cbs;
    u32 group;
    std::array<std::vector<u32>, 18> data;
    std::array<bool, 18> loaded{};
};

class StageEnv : public ShaderEnv {
public:
    StageEnv(ConstReader& cr, TextureSampler* tex, u32 tex_cb) : m_cr(cr), m_tex(tex), m_texCb(tex_cb) {}
    u32 ReadConst(u32 index, u32 offset) override { return m_cr.Read(index, offset); }
    float Interpolate(u32, u32) override { return 0.0f; }
    void SampleTexture(u32 handle, const TextureRequest& req, u32 out[4]) override {
        TextureRequest r = req;
        if (!r.fetch && !r.explicit_lod && !r.grad) { r.explicit_lod = true; r.lod = 0; }
        if (m_tex) m_tex->Sample(handle, r, out);
        else { out[0] = out[1] = out[2] = 0; out[3] = 0x3F800000u; }
    }
    void QueryTexture(u32 handle, u32 lod, u32 out[4]) override {
        if (m_tex) m_tex->Query(handle, lod, out);
        else out[0] = out[1] = out[2] = out[3] = 0;
    }
    u32 TextureConstbuf() override { return m_texCb; }
private:
    ConstReader& m_cr;
    TextureSampler* m_tex;
    u32 m_texCb;
};

// Shader de control: un hilo por punto de control de salida
class TcsEnv final : public StageEnv {
public:
    using StageEnv::StageEnv;
    u32 ReadAttribute(u32 addr) override { return addr < 0x400 && cur < inputs.size() ? inputs[cur]->attr[addr / 4] : 0; }
    void WriteAttribute(u32 addr, u32 value) override { if (addr < 0x400) (*outputs)[cur].attr[addr / 4] = value; }
    u32 ReadAttributeEx(u32 addr, u32 vertex, bool is_patch, bool output) override {
        if (addr >= 0x400) return 0;
        if (is_patch) return (*patch)[addr / 4];
        if (output) return (*outputs)[vertex % outputs->size()].attr[addr / 4];
        return inputs[vertex % inputs.size()]->attr[addr / 4];
    }
    void WriteAttributeEx(u32 addr, u32 value, u32, bool is_patch) override {
        if (addr >= 0x400) return;
        if (is_patch) (*patch)[addr / 4] = value;
        else (*outputs)[cur].attr[addr / 4] = value;
    }
    u32 SystemRegister(u32 sr) override {
        switch (sr) {
            case 0x00: case 0x11: return cur;                 // laneid, invocation_id
            case 0x1D: return u32(inputs.size()) & 0xFF;      // invocation_info: vertices por parche
            default: return 0;
        }
    }
    std::vector<const Vertex*> inputs;
    std::vector<Vertex>* outputs = nullptr;
    std::array<u32, 256>* patch = nullptr;
    u32 cur = 0;
};

// Shader de evaluacion: un hilo por punto del dominio
class TesEnv final : public StageEnv {
public:
    using StageEnv::StageEnv;
    u32 ReadAttribute(u32 addr) override {
        if (addr == 0x2F0) return U(u);
        if (addr == 0x2F4) return U(v);
        return 0;
    }
    void WriteAttribute(u32 addr, u32 value) override { if (addr < 0x400) out->attr[addr / 4] = value; }
    u32 ReadAttributeEx(u32 addr, u32 vertex, bool is_patch, bool) override {
        if (addr == 0x2F0 || addr == 0x2F4) return ReadAttribute(addr);
        if (addr >= 0x400) return 0;
        if (is_patch) return (*patch)[addr / 4];
        return (*cps)[vertex % cps->size()].attr[addr / 4];
    }
    u32 SystemRegister(u32 sr) override {
        if (sr == 0x1D) return u32(cps->size()) & 0xFF;   // invocation_info: puntos de control
        return 0;
    }
    const std::vector<Vertex>* cps = nullptr;
    const std::array<u32, 256>* patch = nullptr;
    Vertex* out = nullptr;
    float u = 0, v = 0;
};
} // namespace

TessDomain Tessellate(u32 prim, u32 spacing, bool cw, const float outer[4], const float inner[2]) {
    TessDomain d;
    const int needed = prim == 0 ? 2 : prim == 1 ? 3 : 4;
    for (int i = 0; i < needed; ++i)
        if (!(outer[i] > 0)) return d;   // nivel exterior <= 0 (o NaN): el parche no se dibuja
    Builder b(d, cw);
    if (prim == 0) TessIsolines(d, spacing, outer);
    else if (prim == 1) TessTriangle(b, spacing, outer, inner);
    else TessQuad(b, spacing, outer, inner);
    return d;
}

bool RunTessellation(Gpu& gpu, const u32* regs, const ConstbufTable& cbs, TextureSampler* tex, u32 tex_cb,
                     const std::vector<s64>& slots, SoftwareRasterizer::VertexStage& out,
                     std::vector<std::array<u32, 2>>& lines) {
    GpuMemoryManager& mm = gpu.MemoryManager();
    auto stage_on = [&](u32 s) { return s == 1 || (regs[PIPELINE + 0x10 * s] & 1); };   // VertexB: siempre
    if (!stage_on(3)) return true;   // sin shader de evaluacion no sale nada
    const bool has_tcs = stage_on(2);
    const u32 P = std::max<u32>(regs[PATCH_SIZE], 1);
    const u32 mode = regs[TESS_MODE];
    const u32 prim = mode & 3, spacing = (mode >> 4) & 3;
    const bool cw = prim != 0 && ((mode >> 8) & 1);

    auto read = [&mm](u64 a, void* d, size_t n) { mm.ReadBlock(a, d, n); };
    const u64 region = Iova(regs[PROGRAM_REGION], regs[PROGRAM_REGION + 1]);
    std::optional<ShaderProgram> tcs;
    if (has_tcs) tcs.emplace(region + regs[PIPELINE + 0x20 + 1], read);
    ShaderProgram tes(region + regs[PIPELINE + 0x30 + 1], read);
    u32 M = has_tcs ? (tcs->Header().words[2] >> 24) & 0xFF : P;
    if (M == 0) M = P;
    auto group = [&](u32 s) { return std::min<u32>(regs[PIPELINE + 0x10 * s + 4] & 7, 4); };
    ConstReader tcs_cb(mm, cbs, has_tcs ? group(2) : 1), tes_cb(mm, cbs, group(3));

    TcsEnv tcs_env(tcs_cb, tex, tex_cb);
    TesEnv tes_env(tes_cb, tex, tex_cb);
    tcs_env.global = tes_env.global = &mm;
    std::vector<ShaderInterpreter> th(has_tcs ? M : 0);
    for (auto& t : th) t.group_mode = true;
    std::vector<u8> state(M);
    ShaderInterpreter tes_interp;
    std::vector<Vertex> cps;
    std::array<u32, 256> patch{};

    size_t start = 0;
    while (start < slots.size()) {
        size_t end = start;
        while (end < slots.size() && slots[end] >= 0) ++end;
        for (size_t p0 = start; p0 + P <= end; p0 += P) {
            // --- Shader de control (o los vertices tal cual) ---
            patch.fill(0);
            std::vector<const Vertex*> inputs;
            for (u32 i = 0; i < P; ++i) inputs.push_back(&out.verts[size_t(slots[p0 + i])]);
            float outer[4], inner[2];
            if (has_tcs) {
                cps.assign(M, Vertex{});
                tcs_env.inputs = inputs;
                tcs_env.outputs = &cps;
                tcs_env.patch = &patch;
                std::fill(state.begin(), state.end(), u8(0));
                u32 done = 0;
                bool progress = true;
                while (done < M && progress) {
                    progress = false;
                    for (u32 i = 0; i < M; ++i) {
                        if (state[i] == 2) continue;
                        tcs_env.cur = i;
                        const auto res = state[i] == 0 ? th[i].Run(*tcs, tcs_env) : th[i].Resume(*tcs, tcs_env);
                        if (!res.ok) { out.failed = true; out.error = "shader de control de teselado: " + res.error; return false; }
                        if (res.barrier) state[i] = 1; else { state[i] = 2; ++done; }
                        progress = true;
                    }
                }
                for (int i = 0; i < 4; ++i) outer[i] = F(patch[size_t(i)]);
                for (int i = 0; i < 2; ++i) inner[i] = F(patch[size_t(4 + i)]);
            } else {
                cps.clear();
                for (const Vertex* v : inputs) cps.push_back(*v);
                for (int i = 0; i < 4; ++i) outer[i] = F(regs[TESS_OUTER + i]);
                for (int i = 0; i < 2; ++i) inner[i] = F(regs[TESS_INNER + i]);
            }

            // --- Teselador ---
            const TessDomain dom = Tessellate(prim, spacing, cw, outer, inner);
            if (dom.points.empty()) continue;

            // --- Shader de evaluacion en cada punto ---
            tes_env.cps = &cps;
            tes_env.patch = &patch;
            const u32 base = u32(out.verts.size());
            for (const auto& pt : dom.points) {
                Vertex& v = out.verts.emplace_back();
                v.attr[0x7C / 4] = U(1.0f);
                tes_env.out = &v;
                tes_env.u = pt[0];
                tes_env.v = pt[1];
                const auto res = tes_interp.Run(tes, tes_env);
                if (!res.ok) { out.failed = true; out.error = "shader de evaluacion de teselado: " + res.error; return false; }
            }
            for (const auto& t : dom.tris) out.tris.push_back({base + t[0], base + t[1], base + t[2], base + t[0]});
            for (const auto& l : dom.lines) lines.push_back({base + l[0], base + l[1]});
        }
        start = end + 1;
    }
    return true;
}

} // namespace NeXo2::GPU
