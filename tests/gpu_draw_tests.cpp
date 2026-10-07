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

using namespace NeXo2;
using NeXo2::Tests::ShaderBin;

namespace {
constexpr u64 CPU_BASE = 0x2000'0000;
constexpr u64 GPU_BASE = 0x4'0000'0000;
constexpr u64 SIZE = 0x100000;
constexpr u64 PB = GPU_BASE;              // pushbuffer
constexpr u64 CODE = GPU_BASE + 0x10000;  // region de programas (SetProgramRegion)
constexpr u64 VB = GPU_BASE + 0x20000;    // vertices
constexpr u64 UBO = GPU_BASE + 0x21000;   // uniform buffers (de 0x100 en 0x100)
constexpr u64 IB = GPU_BASE + 0x22000;    // indices
constexpr u64 RT = GPU_BASE + 0x40000;    // render target 64x64 RGBA8
constexpr u64 ZT = GPU_BASE + 0x60000;    // profundidad 64x64 Z32F
constexpr u32 W = 64, H = 64;

u32 Hdr(u32 mode, u32 count, u32 method) { return (method & 0x1FFF) | ((count & 0x1FFF) << 16) | (mode << 29); }
u32 F(float f) { u32 x; std::memcpy(&x, &f, 4); return x; }
u32 Hi(u64 v) { return u32(v >> 32); }
u32 Lo(u64 v) { return u32(v); }

struct Push {
    std::vector<u32> w;
    void Cmd(u32 method, std::initializer_list<u32> args) {
        w.push_back(Hdr(1, u32(args.size()), method));
        w.insert(w.end(), args);
    }
};

struct DrawFixture {
    Core::Memory mem;
    GPU::Gpu gpu{mem};
    GPU::Channel& ch;
    u32 vs_offset = 0, fs_offset = 0;

    DrawFixture() : ch(gpu.CreateChannel()) {
        mem.WriteBytes(CPU_BASE, std::vector<u8>(SIZE, 0).data(), SIZE);
        gpu.MemoryManager().Map(GPU_BASE, CPU_BASE, SIZE);
    }
    void Run(const Push& p) {
        gpu.MemoryManager().WriteBlock(PB, p.w.data(), p.w.size() * 4);
        ch.ProcessPushbuffer(PB, u32(p.w.size()));
    }
    void Write(u64 va, const void* data, size_t n) { gpu.MemoryManager().WriteBlock(va, data, n); }

    // Sube los dos shaders y deja el estado basico como lo deja deko3d
    void Setup(Push& p, const ShaderBin& vs, const ShaderBin& fs, bool block_linear = true) {
        Write(CODE, vs.code, vs.size);
        Write(CODE + 0x1000, fs.code, fs.size);
        vs_offset = vs.entry;
        fs_offset = 0x1000 + fs.entry;
        p.Cmd(0, {0xB197});                                          // clase 3D en la subcanal 0
        p.Cmd(0x582, {Hi(CODE), Lo(CODE)});                          // SetProgramRegion
        p.Cmd(0x810, {1u | (1u << 4), vs_offset});                   // SetProgram[VertexB]
        p.Cmd(0x814, {0});                                           //   grupo de enlaces 0
        p.Cmd(0x850, {1u | (5u << 4), fs_offset});                   // SetProgram[Fragment]
        p.Cmd(0x854, {4});                                           //   grupo 4
        // Render target 0: RGBA8 (0xD5), en bloques (como deko3d) o lineal
        p.Cmd(0x200, {Hi(RT), Lo(RT), block_linear ? W : W * 4, H, 0xD5, block_linear ? 0u : (1u << 12), 1, 0});
        p.Cmd(0x487, {1u | (076543210u << 4)});
        p.Cmd(0x3FD, {W << 16, H << 16});
        // Viewport como deko3d (origen arriba a la izquierda, y de NDC hacia arriba)
        p.Cmd(0x280, {F(W / 2.0f), F(-(H / 2.0f)), F(1.0f), F(W / 2.0f), F(H / 2.0f), F(0.0f)});
        p.Cmd(0x300, {W << 16, H << 16, F(0.0f), F(1.0f)});
        p.Cmd(0x64B, {1});                                           // ViewportTransformEnable
        p.Cmd(0x35F, {1});                                           // profundidad 0..1
        p.Cmd(0x680, {0x1111});                                      // escribir RGBA
        // Vertices: stream 0 con posicion (3 floats) y color (3 floats)
        p.Cmd(0x700, {24u | (1u << 12), Hi(VB), Lo(VB)});
        p.Cmd(0x458, {(0x02u << 21) | (7u << 27), (12u << 7) | (0x02u << 21) | (7u << 27)});
    }
    void Clear(Push& p, float r, float g, float b, float a) {
        p.Cmd(0x360, {F(r), F(g), F(b), F(a)});
        p.Cmd(0x674, {0xFu << 2});
    }
    void DrawArrays(Push& p, u32 topology, u32 first, u32 count) {
        p.Cmd(0x586, {topology});
        p.Cmd(0x35D, {first, count});   // DrawArraysFirst, DrawArraysCount (dibuja)
        p.Cmd(0x585, {0});
    }
    // Uniform buffer 'slot' (c[2 + slot]) para la etapa 'stage' (0 vertices, 4 pixeles)
    void BindUbo(Push& p, u32 stage, u64 addr, u32 size) {
        p.Cmd(0x8E0, {size, Hi(addr), Lo(addr)});
        p.Cmd(0x904 + 8 * stage, {1u | (2u << 4)});
    }
    std::array<u8, 4> Pixel(u32 x, u32 y, bool block_linear = true) {
        std::array<u8, 4> px{};
        const u64 off = block_linear ? GPU::BlockLinearOffset(x * 4, y, W * 4, 0) : u64(y) * W * 4 + x * 4;
        gpu.MemoryManager().ReadBlock(RT + off, px.data(), 4);
        return px;
    }
    u32 CountNonBlack(bool block_linear = true) {
        u32 n = 0;
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W; ++x) { auto p = Pixel(x, y, block_linear); if (p[0] || p[1] || p[2]) ++n; }
        return n;
    }
};

struct V { float x, y, z, r, g, b; };
// Triangulo grande: abajo-izquierda rojo, abajo-derecha verde, arriba-centro azul (sentido CCW)
const V kTri[3] = {{-1, -1, 0.5f, 1, 0, 0}, {1, -1, 0.5f, 0, 1, 0}, {0, 1, 0.5f, 0, 0, 1}};

bool Near(int a, int b, int tol = 3) { return std::abs(a - b) <= tol; }
} // namespace

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
