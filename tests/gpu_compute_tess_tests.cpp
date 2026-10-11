// Tests de compute (QMD, memoria compartida, barrier, atomicos), teselado (TCS + teselador
// + TES) y de puntos, lineas y modo de poligono. Shaders reales compilados con uam
// (tests/shaders/groups.comp, tess.tesc, tess.tese).
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "test_framework.hpp"
#include "video_core/tessellation.hpp"

#include "gpu_draw_fixture.hpp"

namespace {
constexpr u64 QMD = GPU_BASE + 0x30000;    // la QMD del dispatch
constexpr u64 CB0 = GPU_BASE + 0x30100;    // constbuf del driver (c[0])
constexpr u64 SSBO = GPU_BASE + 0x31000;   // buffer de almacenamiento

// Area con signo (en el plano u, v) de todos los triangulos del dominio
double DomainArea(const GPU::TessDomain& d, bool& all_ccw) {
    double total = 0;
    all_ccw = true;
    for (const auto& t : d.tris) {
        const auto &a = d.points[t[0]], &b = d.points[t[1]], &c = d.points[t[2]];
        const double area = 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]));
        if (area <= 0) all_ccw = false;
        total += area;
    }
    return total;
}
} // namespace

TEST(Compute_SharedMemoryBarrierAtomics) {
    DrawFixture f;
    f.Write(CODE, Tests::k_groups_comp.code, Tests::k_groups_comp.size);
    // c[0] como lo deja deko3d: tamano del grupo, numero de grupos y el buffer 0 en 0x140
    std::vector<u32> cb0(0x200 / 4, 0);
    cb0[0] = 64; cb0[1] = 1; cb0[2] = 1;
    cb0[3] = 3; cb0[4] = 1; cb0[5] = 1;
    cb0[0x140 / 4] = Lo(SSBO);
    cb0[0x144 / 4] = Hi(SSBO);
    cb0[0x148 / 4] = 0x1000;
    f.Write(CB0, cb0.data(), cb0.size() * 4);
    std::vector<u32> clear(0x1000 / 4, 0xDEADBEEF);
    clear[0] = 0;
    f.Write(SSBO, clear.data(), clear.size() * 4);
    // QMD: programa en el offset 0, 3 grupos de 64 hilos, 256 bytes compartidos, c[0]
    u32 q[64] = {};
    q[8] = Tests::k_groups_comp.entry;
    q[12] = 3;
    q[13] = 1u | (1u << 16);
    q[17] = 256;
    q[18] = 64u << 16;
    q[19] = 1u | (1u << 16);
    q[20] = 1;
    q[29] = Lo(CB0);
    q[30] = Hi(CB0) | (0x200u << 15);
    f.Write(QMD, q, sizeof(q));

    Push p;
    p.Cmd(0, {0xB1C0});                       // clase de compute
    p.Cmd(0x582, {Hi(CODE), Lo(CODE)});       // SetProgramRegion
    p.Cmd(0xAD, {u32(QMD >> 8)});             // SendPcasA
    p.Cmd(0xAF, {0x6});                       // SendSignalingPcasB: lanzar
    f.Run(p);

    CHECK_EQ(f.gpu.GetStats().dispatches, 1ULL);
    CHECK_EQ(f.gpu.GetStats().compute_threads, 192ULL);
    CHECK_EQ(f.gpu.GetStats().shader_errors, 0ULL);
    std::vector<u32> out(4 + 192);
    f.gpu.MemoryManager().ReadBlock(SSBO, out.data(), out.size() * 4);
    CHECK_EQ(out[0], 192u);                   // atomicAdd de los 192 hilos
    bool ok = true;
    for (u32 g = 0; g < 3; ++g)
        for (u32 i = 0; i < 64; ++i)          // cada hilo lee lo que escribio otro (tras el barrier)
            if (out[4 + g * 64 + i] != (63 - i) * (63 - i) + g * 1000) ok = false;
    CHECK(ok);
}

TEST(Tess_DomainShapes) {
    bool ccw = false;
    const float ones[4] = {1, 1, 1, 1}, in1[2] = {1, 1};
    auto t1 = GPU::Tessellate(1, 0, false, ones, in1);
    CHECK_EQ(t1.tris.size(), size_t(1));                       // todo a 1: un triangulo
    auto q1 = GPU::Tessellate(2, 0, false, ones, in1);
    CHECK_EQ(q1.tris.size(), size_t(2));                       // un cuadrado
    const float three[4] = {3, 3, 3, 3}, in3[2] = {3, 3};
    auto t3 = GPU::Tessellate(1, 0, false, three, in3);
    CHECK_EQ(t3.tris.size(), size_t(13));                      // anillo de 3 + triangulo central
    const float iso[4] = {3, 4, 0, 0};
    auto l = GPU::Tessellate(0, 0, false, iso, in1);
    CHECK_EQ(l.lines.size(), size_t(12));                      // 3 lineas de 4 tramos
    const float zero[4] = {0, 1, 1, 1};
    CHECK(GPU::Tessellate(1, 0, false, zero, in3).tris.empty());   // nivel exterior 0: fuera

    // Muchas combinaciones: el dominio queda cubierto entero, sin huecos ni solapes, y todos
    // los triangulos con el mismo sentido
    const float levels[] = {1, 1.5f, 2, 2.2f, 3, 4.7f, 5, 8};
    bool areas_ok = true, winding_ok = true;
    for (u32 prim : {1u, 2u})
        for (u32 spacing : {0u, 1u, 2u})
            for (float a : levels)
                for (float b : levels)
                    for (float c : levels) {
                        const float outer[4] = {a, b, c, a + b}, inner[2] = {c, a};
                        auto d = GPU::Tessellate(prim, spacing, false, outer, inner);
                        const double area = DomainArea(d, ccw);
                        if (std::fabs(area - (prim == 1 ? 0.5 : 1.0)) > 1e-4) areas_ok = false;
                        if (!ccw) winding_ok = false;
                        auto cw = GPU::Tessellate(prim, spacing, true, outer, inner);
                        if (std::fabs(DomainArea(cw, ccw) + (prim == 1 ? 0.5 : 1.0)) > 1e-4) winding_ok = false;
                    }
    CHECK(areas_ok);
    CHECK(winding_ok);
}

TEST(Draw_TessellationMatchesPlainTriangle) {
    // El mismo triangulo, sin teselar y teselado con un TES que solo interpola: misma imagen
    std::vector<std::array<u8, 4>> plain;
    {
        DrawFixture f;
        f.Write(VB, kTri, sizeof(kTri));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
        f.Clear(p, 0, 0, 0, 1);
        f.DrawArrays(p, 4, 0, 3);
        f.Run(p);
        for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W; ++x) plain.push_back(f.Pixel(x, y));
    }
    DrawFixture f;
    f.Write(VB, kTri, sizeof(kTri));
    f.Write(CODE + 0x2000, Tests::k_tess_tess_ctrl.code, Tests::k_tess_tess_ctrl.size);
    f.Write(CODE + 0x3000, Tests::k_tess_tess_eval.code, Tests::k_tess_tess_eval.size);
    Push p;
    f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
    p.Cmd(0x820, {1u | (2u << 4), 0x2000 + Tests::k_tess_tess_ctrl.entry});   // SetProgram[TessCtrl]
    p.Cmd(0x824, {1});
    p.Cmd(0x830, {1u | (3u << 4), 0x3000 + Tests::k_tess_tess_eval.entry});   // SetProgram[TessEval]
    p.Cmd(0x834, {2});
    p.Cmd(0x0C8, {0x201});                    // triangulos, espaciado igual, ccw
    p.Cmd(0x373, {3});                        // parches de 3 vertices
    f.Clear(p, 0, 0, 0, 1);
    f.DrawArrays(p, 14, 0, 3);
    f.Run(p);

    const float outer[4] = {4, 2, 3, 0}, inner[2] = {3, 0};
    const size_t expected = GPU::Tessellate(1, 0, false, outer, inner).tris.size();
    CHECK_EQ(f.gpu.GetStats().shader_errors, 0ULL);
    CHECK_EQ(f.gpu.GetStats().triangles, u64(expected));
    int worst = 0;
    size_t i = 0;
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x, ++i) {
            const auto px = f.Pixel(x, y);
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(px[c]) - int(plain[i][c])));
        }
    if (worst > 2) std::printf("    diferencia maxima con el triangulo sin teselar: %d\n", worst);
    CHECK(worst <= 2);
}

TEST(Draw_WideLinesPointsAndWireframe) {
    // Linea horizontal de 4 pixeles de ancho, de x = 8 a x = 56 (en un 64x64)
    {
        DrawFixture f;
        const V line[2] = {{-0.75f, 0, 0.5f, 1, 1, 1}, {0.75f, 0, 0.5f, 1, 1, 1}};
        f.Write(VB, line, sizeof(line));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
        p.Cmd(0x4ED, {F(4.0f)});              // LineWidthAliased
        f.Clear(p, 0, 0, 0, 1);
        f.DrawArrays(p, 1, 0, 2);
        f.Run(p);
        CHECK_EQ(f.CountNonBlack(), 48u * 4u);
    }
    // Dos puntos de 6x6
    {
        DrawFixture f;
        const V pts[2] = {{-0.5f, -0.5f, 0.5f, 1, 0, 0}, {0.5f, 0.5f, 0.5f, 0, 1, 0}};
        f.Write(VB, pts, sizeof(pts));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
        p.Cmd(0x546, {F(6.0f)});              // PointSpriteSize
        f.Clear(p, 0, 0, 0, 1);
        f.DrawArrays(p, 0, 0, 2);
        f.Run(p);
        CHECK_EQ(f.CountNonBlack(), 2u * 36u);
    }
    // Modo de poligono "lineas": solo el borde del triangulo
    {
        DrawFixture f;
        f.Write(VB, kTri, sizeof(kTri));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
        p.Cmd(0x36B, {0x1B01, 0x1B01});       // SetPolygonModeFront/Back = Line
        f.Clear(p, 0, 0, 0, 1);
        f.DrawArrays(p, 4, 0, 3);
        f.Run(p);
        const u32 n = f.CountNonBlack();
        CHECK(n > 64 && n < 600);             // el borde, no el triangulo entero (~2000)
        CHECK(f.Pixel(32, 40)[0] == 0 && f.Pixel(32, 40)[1] == 0);   // por dentro, vacio
    }
}
