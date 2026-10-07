// Tests del interprete de shaders Maxwell (video_core/shader.hpp).
//
// - Programas pequenos compilados con uam (el compilador de deko3d): tests/generated/
//   shader_bins.hpp, generado por tools/shader_fuzz.py --fixed.
// - Pruebas al azar: tests/generated/shader_fuzz.bin (tools/shader_fuzz.py). Cada una es
//   un shader de vertices con 16 expresiones y su resultado esperado calculado en Python.
// Ver docs/07-nexo-internals/gpu-shaders.md.
#include "test_framework.hpp"
#include "video_core/shader.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace NeXo2;
using namespace NeXo2::GPU;

namespace {

float AsF(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
u32 AsU(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }

// Un .dksh (formato de modulo de shader de deko3d): cabecera + codigo
struct Dksh {
    std::vector<u8> code;   // seccion de codigo (empieza con relleno, luego SPH + instrucciones)
    u32 entry = 0;          // offset de la SPH dentro del codigo
    std::vector<u32> cb1;   // constantes que genera el compilador (van en c[1])
};

bool ParseDksh(const u8* data, size_t size, Dksh& out) {
    if (size < 24) return false;
    u32 h[6];
    std::memcpy(h, data, 24);
    if (h[0] != 0x48534B44 || h[4] + 64 > size || h[2] > size) return false;   // "DKSH"
    u32 ph[16];
    std::memcpy(ph, data + h[4], 64);
    out.code.assign(data + h[2], data + size);
    out.entry = ph[1];
    const u32 cb1_off = ph[3], cb1_size = ph[4];
    out.cb1.assign(cb1_size / 4, 0);
    if (cb1_size && cb1_off + cb1_size <= out.code.size()) std::memcpy(out.cb1.data(), &out.code[cb1_off], cb1_size);
    return true;
}

ShaderProgram MakeProgram(const Dksh& d) {
    return ShaderProgram(d.entry, [&d](u64 addr, void* dst, size_t n) {
        std::memset(dst, 0, n);
        if (addr < d.code.size()) std::memcpy(dst, &d.code[addr], std::min<size_t>(n, d.code.size() - addr));
    });
}

// Entorno de pruebas: constbufs en memoria, atributos en mapas
struct TestEnv : ShaderEnv {
    std::map<u32, std::vector<u32>> cb;
    std::map<u32, u32> in, out;
    std::map<u32, float> interp;
    u32 ReadConst(u32 i, u32 o) override {
        auto it = cb.find(i);
        return it != cb.end() && o / 4 < it->second.size() ? it->second[o / 4] : 0;
    }
    u32 ReadAttribute(u32 a) override { auto it = in.find(a); return it != in.end() ? it->second : 0; }
    void WriteAttribute(u32 a, u32 v) override { out[a] = v; }
    float Interpolate(u32 a, u32) override { auto it = interp.find(a); return it != interp.end() ? it->second : 0.0f; }
};

} // namespace

// --- Decodificacion: instrucciones sacadas de shaders reales de uam ---
TEST(Shader_Decode) {
    // ipa pass $r0 a[0x7c]
    ShaderInstr i = DecodeShaderInstr(0xE003FF87CFF7FF00ull);
    CHECK(i.op == ShOp::Ipa);
    CHECK_EQ(i.rd, 0);
    CHECK_EQ(i.Bits(28, 10), 0x7C);
    CHECK_EQ(i.Bits(54, 2), 0);
    // mufu rcp $r2 $r0
    i = DecodeShaderInstr(0x5080000000470002ull);
    CHECK(i.op == ShOp::Mufu);
    CHECK_EQ(i.Bits(20, 4), 4);
    // mov32i $r3 0x3f800000
    i = DecodeShaderInstr(0x0103F8000007F003ull);
    CHECK(i.op == ShOp::Mov32i);
    CHECK_EQ(i.imm, 0x3F800000u);
    CHECK_EQ(i.rd, 3);
    // fmul ftz $r2 $r0 c2[0x0]  (forma constbuf)
    i = DecodeShaderInstr(0x4C68100800070002ull);
    CHECK(i.op == ShOp::Fmul);
    CHECK(i.form == ShForm::Cbuf);
    CHECK_EQ(i.cb_index, 2);
    CHECK_EQ(i.cb_offset, 0);
    // ffma ftz $r2 $r1 c2[0x10] $r2
    i = DecodeShaderInstr(0x49A0010800470102ull);
    CHECK(i.op == ShOp::Ffma);
    CHECK_EQ(i.cb_offset, 0x10);
    CHECK_EQ(i.rc, 2);
    // ffma ftz $r2 $r2 0x3f000000 $r6  (inmediato float de 19 bits)
    i = DecodeShaderInstr(0x32A0033F00070202ull);
    CHECK(i.op == ShOp::Ffma);
    CHECK(i.form == ShForm::Imm);
    CHECK_EQ(i.imm, 0x3F000000u);
    CHECK_EQ(i.rc, 6);
    // iadd $r3 $r3 -0x1  (inmediato entero negativo: el signo va en el bit 56)
    i = DecodeShaderInstr(0x3910007FFFF70303ull);
    CHECK(i.op == ShOp::Iadd);
    CHECK_EQ(i.imm, 0xFFFFFFFFu);
    // $p0 kil  (predicado P0 = bits 16..18 a cero)
    i = DecodeShaderInstr(0xE33000000000000Full);
    CHECK(i.op == ShOp::Kil);
    CHECK_EQ(i.pred, 0);
    CHECK(!i.pred_neg);
    // pbk: no lleva predicado (esos bits son parte del destino) -> siempre PT
    i = DecodeShaderInstr(0xE2A0000005800000ull);
    CHECK(i.op == ShOp::Pbk);
    CHECK_EQ(i.pred, 7);
    // xmad psl cbcc $r0 h1 $r0 h1 $r3 $r1
    i = DecodeShaderInstr(0x5B30009800370000ull);
    CHECK(i.op == ShOp::Xmad);
    CHECK(i.form == ShForm::Reg);
    // Palabra que no es ninguna instruccion conocida
    CHECK(DecodeShaderInstr(0xFFFFFFFFFFFFFFFFull).op == ShOp::Invalid);
}

// --- Pruebas al azar: comparar con el resultado calculado en Python ---
TEST(Shader_Fuzz) {
    const char* file_env = std::getenv("NEXO2_SHADER_FUZZ");
    std::ifstream f(file_env ? std::string(file_env) : std::string(NEXO2_TEST_DATA_DIR) + "/shader_fuzz.bin",
                    std::ios::binary);
    CHECK(f.good());
    if (!f.good()) return;
    std::vector<u8> blob((std::istreambuf_iterator<char>(f)), {});
    size_t pos = 0;
    auto rd = [&]() { u32 v = 0; if (pos + 4 <= blob.size()) std::memcpy(&v, &blob[pos], 4); pos += 4; return v; };
    CHECK_EQ(rd(), 0x4653584Eu);   // "NXSF"
    CHECK_EQ(rd(), 1u);
    const u32 count = rd();
    CHECK(count > 0);

    u32 failures = 0, outputs = 0, programs_ok = 0;
    for (u32 n = 0; n < count && pos < blob.size(); ++n) {
        const u32 glsl_len = rd();
        const std::string glsl(reinterpret_cast<const char*>(&blob[pos]), glsl_len);
        pos += (glsl_len + 3) & ~3u;
        const u32 dksh_len = rd();
        Dksh d;
        const bool parsed = ParseDksh(&blob[pos], dksh_len, d);
        pos += (dksh_len + 3) & ~3u;
        std::vector<u32> ubo(32);
        for (auto& w : ubo) w = rd();
        struct Out { u32 kind, exp, alt; float tol; } outs[16];
        for (auto& o : outs) { o.kind = rd(); o.exp = rd(); o.alt = rd(); u32 t = rd(); std::memcpy(&o.tol, &t, 4); }
        if (!parsed) { ++failures; continue; }

        TestEnv env;
        env.cb[1] = d.cb1;
        env.cb[2] = ubo;   // uniform buffer 0 -> c[2] en deko3d
        ShaderProgram prog = MakeProgram(d);
        ShaderInterpreter si;
        const auto res = si.Run(prog, env);
        if (!res.ok) {
            if (++failures <= 5) std::printf("    prueba %u: %s\n%s\n", n, res.error.c_str(), glsl.c_str());
            continue;
        }
        bool bad = false;
        for (u32 k = 0; k < 16; ++k) {
            ++outputs;
            const u32 got = env.out[0x80 + 4 * k];   // of0, of1, oi0, oi1 -> a[0x80..0xBC]
            const Out& o = outs[k];
            bool ok;
            if (o.kind == 1) ok = got == o.exp;
            else {
                const float g = AsF(got);
                ok = got == o.exp || got == o.alt ||
                     std::fabs(g - AsF(o.exp)) <= o.tol || std::fabs(g - AsF(o.alt)) <= o.tol;
            }
            if (!ok) {
                if (!bad && failures < 5)
                    std::printf("    prueba %u, salida %u: 0x%08X (%g), esperado 0x%08X (%g)\n%s\n", n, k, got, AsF(got),
                                o.exp, AsF(o.exp), glsl.c_str());
                bad = true;
            }
        }
        if (bad) ++failures; else ++programs_ok;
    }
    std::printf("    %u programas, %u salidas: %u programas bien\n", count, outputs, programs_ok);
    CHECK_EQ(failures, 0u);
}
