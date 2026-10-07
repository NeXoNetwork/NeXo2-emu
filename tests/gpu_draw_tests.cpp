// Tests del dibujo por software (GPU fase 2): triangulos con shaders Maxwell reales
// (tests/shaders, compilados con uam), profundidad, descartes, mezcla y culling.
// Los comandos son los mismos que envia deko3d.
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "test_framework.hpp"
#include "memory.hpp"
#include "system.hpp"
#include "video_core/gpu.hpp"
#include "video_core/engines.hpp"
#include "video_core/surface.hpp"
#include "generated/shader_bins.hpp"

#include "gpu_draw_fixture.hpp"


TEST(Draw_TriangleColorsAndCoverage) {
    DrawFixture f;
    f.Write(VB, kTri, sizeof(kTri));
    Push p;
    f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
    f.Clear(p, 0, 0, 0, 1);
    f.DrawArrays(p, 4, 0, 3);
    f.Run(p);

    auto& st = f.gpu.GetStats();
    CHECK_EQ(st.draws, 1u);
    CHECK_EQ(st.shader_errors, 0u);
    CHECK_EQ(st.triangles, 1u);
    // Area: medio cuadrado de 64x64 = 2048 pixeles (con la regla de bordes, exacto +-1 fila)
    const u32 n = f.CountNonBlack();
    CHECK(n >= 2016 && n <= 2080);
    CHECK_EQ(n, u32(st.pixels));
    // Esquinas de arriba: fuera del triangulo (negro)
    CHECK_EQ(f.Pixel(0, 0)[0], 0);
    CHECK_EQ(f.Pixel(63, 0)[1], 0);
    // Abajo a la izquierda: casi todo rojo; abajo a la derecha: verde; arriba: azul
    auto bl = f.Pixel(1, 62), br = f.Pixel(62, 62), top = f.Pixel(32, 2);
    CHECK(bl[0] > 230 && bl[1] < 20 && bl[2] < 20);
    CHECK(br[1] > 230 && br[0] < 20 && br[2] < 20);
    CHECK(top[2] > 220 && top[0] < 20 && top[1] < 20);
    // Centro de gravedad (32, 42.7): un tercio de cada color
    auto c = f.Pixel(32, 42);
    CHECK(Near(c[0], 85, 6) && Near(c[1], 85, 6) && Near(c[2], 85, 6));
    CHECK_EQ(c[3], 255);
}

TEST(Draw_CullingAndWinding) {
    // deko3d por defecto: cara delantera = CCW, se descartan las traseras
    for (int reversed = 0; reversed < 2; ++reversed) {
        DrawFixture f;
        V t[3] = {kTri[0], kTri[1], kTri[2]};
        if (reversed) std::swap(t[1], t[2]);   // ahora gira al reves (CW): cara trasera
        f.Write(VB, t, sizeof(t));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
        f.Clear(p, 0, 0, 0, 1);
        p.Cmd(0x646, {1, 0x901, 0x405});   // culling activo, delantera CCW, quitar traseras
        f.DrawArrays(p, 4, 0, 3);
        f.Run(p);
        if (reversed) CHECK_EQ(f.CountNonBlack(), 0u);
        else CHECK(f.CountNonBlack() > 2000);
    }
}

TEST(Draw_IndexedStripLinearTargetNoGaps) {
    // Un cuadrado de 4 vertices como tira de triangulos con indices de 16 bits, en un
    // render target lineal. Los dos triangulos comparten una diagonal: no puede quedar
    // ningun hueco ni pixel pintado dos veces.
    DrawFixture f;
    const V quad[4] = {{-0.5f, -0.5f, 0, 1, 1, 1}, {0.5f, -0.5f, 0, 1, 1, 1},
                       {-0.5f, 0.5f, 0, 1, 1, 1}, {0.5f, 0.5f, 0, 1, 1, 1}};
    f.Write(VB, quad, sizeof(quad));
    const u16 idx[6] = {7, 7, 0, 1, 2, 3};   // los dos primeros no se usan (first = 2)
    f.Write(IB, idx, sizeof(idx));
    Push p;
    f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag, false);
    f.Clear(p, 0, 0, 0, 1);
    p.Cmd(0x5F2, {Hi(IB), Lo(IB), Hi(IB + 0x1000), Lo(IB + 0x1000), 1});   // indices u16
    p.Cmd(0x586, {5});                      // tira de triangulos
    p.Cmd(0x5F7, {2, 4});                   // DrawElementsFirst = 2, DrawElementsCount = 4
    p.Cmd(0x585, {0});
    f.Run(p);
    // El cuadrado va de x=16 a x=48 y de y=16 a y=48: exactamente 32x32 pixeles blancos
    CHECK_EQ(f.CountNonBlack(false), 32u * 32u);
    CHECK_EQ(f.gpu.GetStats().pixels, 32u * 32u);
    CHECK_EQ(f.Pixel(16, 16, false)[0], 255);
    CHECK_EQ(f.Pixel(47, 47, false)[1], 255);
    CHECK_EQ(f.Pixel(48, 47, false)[0], 0);
    CHECK_EQ(f.Pixel(15, 20, false)[0], 0);
}

TEST(Draw_DepthTest) {
    // Dos triangulos que se tapan, en los dos ordenes: siempre gana el mas cercano (z menor)
    for (int order = 0; order < 2; ++order) {
        DrawFixture f;
        V tris[6];
        for (int i = 0; i < 3; ++i) {
            tris[i] = kTri[i]; tris[i].z = 0.25f; tris[i].r = 1; tris[i].g = 0; tris[i].b = 0;      // cerca: rojo
            tris[3 + i] = kTri[i]; tris[3 + i].z = 0.75f; tris[3 + i].r = 0; tris[3 + i].g = 1; tris[3 + i].b = 0;   // lejos: verde
        }
        f.Write(VB, tris, sizeof(tris));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_color_frag);
        // Profundidad Z32F (0x0A) de 64x64, borrada a 1.0, prueba "menor que"
        p.Cmd(0x3F8, {Hi(ZT), Lo(ZT), 0x0A, 0, 0});
        p.Cmd(0x48A, {W, H});
        p.Cmd(0x54E, {1});
        p.Cmd(0x4B3, {1});            // DepthTestEnable
        p.Cmd(0x4BA, {1});            // DepthWriteEnable
        p.Cmd(0x4C3, {0x201});        // LESS
        f.Clear(p, 0, 0, 0, 1);
        p.Cmd(0x364, {F(1.0f)});
        p.Cmd(0x674, {1});            // borrar profundidad
        f.DrawArrays(p, 4, order ? 3 : 0, 3);
        f.DrawArrays(p, 4, order ? 0 : 3, 3);
        f.Run(p);
        auto c = f.Pixel(32, 40);
        CHECK(c[0] == 255 && c[1] == 0);
        float z = 0;
        f.gpu.MemoryManager().ReadBlock(ZT + GPU::BlockLinearOffset(32 * 4, 40, W * 4, 0), &z, 4);
        CHECK(std::fabs(z - 0.25f) < 1e-6f);
    }
}

TEST(Draw_DiscardAndUniformBlend) {
    // discard.frag: solo quedan los pixeles con rojo >= 0.5 (la zona cerca del vertice rojo)
    {
        DrawFixture f;
        f.Write(VB, kTri, sizeof(kTri));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_discard_frag);
        f.Clear(p, 0, 0, 0, 1);
        f.DrawArrays(p, 4, 0, 3);
        f.Run(p);
        const u32 n = f.CountNonBlack();
        CHECK(n > 300 && n < 700);          // la cuarta parte del triangulo (~512)
        CHECK(f.Pixel(1, 62)[0] > 230);     // junto al vertice rojo: dibujado
        CHECK_EQ(f.Pixel(62, 62)[1], 0);    // junto al verde: descartado
    }
    // tint.frag: color del uniform buffer (c[2] del shader de pixeles) mezclado al 50 %
    {
        DrawFixture f;
        f.Write(VB, kTri, sizeof(kTri));
        const float color[4] = {0.0f, 0.0f, 1.0f, 0.5f};
        f.Write(UBO, color, sizeof(color));
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_tint_frag);
        f.Clear(p, 1, 1, 1, 1);             // fondo blanco
        f.BindUbo(p, 4, UBO, 0x100);
        p.Cmd(0x4D8, {1});                  // mezcla en el RT 0
        // global: no separado, ADD, SRC_ALPHA, ONE_MINUS_SRC_ALPHA (y lo mismo para alfa)
        p.Cmd(0x4CF, {0, 0x8006, 0x4302, 0x4303, 0x8006, 0x4302, 0, 0x4303});
        f.DrawArrays(p, 4, 0, 3);
        f.Run(p);
        auto c = f.Pixel(32, 40);
        CHECK(Near(c[0], 128) && Near(c[1], 128) && c[2] == 255);
        CHECK_EQ(f.Pixel(0, 0)[0], 255);    // fuera: sigue blanco
    }
}

TEST(Draw_PerspectiveFromUniformMatrix) {
    // xform.vert: posicion = matriz * vertice; con w = 2 el triangulo sale a la mitad de
    // tamano. El tinte (0.5, 1, 1) reduce el rojo a la mitad.
    DrawFixture f;
    f.Write(VB, kTri, sizeof(kTri));
    float u[20] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 2,   // columnas (std140)
                   0.5f, 1, 1, 1};
    f.Write(UBO, u, sizeof(u));
    Push p;
    f.Setup(p, Tests::k_xform_vert, Tests::k_color_frag);
    f.Clear(p, 0, 0, 0, 1);
    f.BindUbo(p, 0, UBO, 0x100);
    f.DrawArrays(p, 4, 0, 3);
    f.Run(p);
    const u32 n = f.CountNonBlack();
    CHECK(n >= 480 && n <= 545);            // un cuarto del area: 512
    auto bl = f.Pixel(17, 46);              // junto al vertice rojo, que ahora esta en (16, 48)
    CHECK(Near(bl[0], 128, 12) && bl[1] < 30);
    CHECK_EQ(f.Pixel(5, 60)[0], 0);         // donde antes llegaba el triangulo grande
}

// Homebrew completo (tests/programs/nro_triangle): nvdrv, canal, pushbuffer con shaders
// de uam y un draw; el propio programa comprueba los pixeles y lo cuenta por la salida.
TEST(Draw_NroTriangle) {
    std::ifstream fin(std::string(NEXO2_TEST_DATA_DIR) + "/triangle.nro", std::ios::binary);
    std::vector<u8> nro((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    CHECK(!nro.empty());
    if (nro.empty()) return;
    Core::System sys;
    CHECK(sys.LoadNro(nro, "triangle.nro"));
    sys.Run(50'000'000);
    const std::string out = sys.GetKernel().GetDebugOutput();
    CHECK(out.find("triangulo completo, fallos: 0") != std::string::npos);
    if (out.find("fallos: 0") == std::string::npos) std::printf("%s\n", out.c_str());
    CHECK_EQ(sys.GetKernel().GetGpu().GetStats().draws, 1u);
    CHECK_EQ(sys.GetKernel().GetGpu().GetStats().triangles, 1u);
}
