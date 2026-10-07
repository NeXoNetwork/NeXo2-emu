// Tests de texturas (GPU fase 2b): descriptores TIC/TSC escritos con el formato de las
// cabeceras publicas de NVIDIA, shaders reales de tests/shaders (TEXS, TLDS, TEX, TXD, TXQ),
// formatos, block linear y lineal, mipmaps, arrays, filtros, repeticion y sombras.
#include <cmath>
#include <cstring>
#include <vector>
#include "test_framework.hpp"
#include "gpu_draw_fixture.hpp"
#include "video_core/texture.hpp"

namespace {
constexpr u64 TIC = GPU_BASE + 0x30000;     // tabla de imagenes (32 bytes cada una)
constexpr u64 TSC = GPU_BASE + 0x31000;     // tabla de samplers
constexpr u64 DRV = GPU_BASE + 0x23000;     // constbuf del driver (c[0]): handles de texturas
constexpr u64 TEXMEM = GPU_BASE + 0x80000;  // texeles

// Pone 'len' bits de 'v' en la posicion 'pos' de un descriptor de 256 bits
void Set(std::array<u32, 8>& w, u32 pos, u32 len, u32 v) {
    for (u32 i = 0; i < len; ++i) {
        const u32 bit = pos + i;
        if ((v >> i) & 1) w[bit / 32] |= 1u << (bit % 32);
        else w[bit / 32] &= ~(1u << (bit % 32));
    }
}

struct TicDesc {
    u64 addr = TEXMEM;
    u32 format = 0x08;                 // A8B8G8R8
    u32 type_r = 2, type_rest = 2;     // 2 = unorm (R y el resto)
    u32 swz[4] = {2, 3, 4, 5};         // X=R, Y=G, Z=B, W=A
    u32 width = 1, height = 1, depth = 1, levels = 1;
    u32 type = 1;                      // 2D
    bool block_linear = true;
    u32 block_height_log2 = 0;
    u32 pitch = 0;
};

std::array<u32, 8> MakeTic(const TicDesc& d) {
    std::array<u32, 8> w{};
    Set(w, 0, 7, d.format);
    Set(w, 7, 3, d.type_r);
    for (u32 c = 1; c < 4; ++c) Set(w, 7 + 3 * c, 3, d.type_rest);
    for (u32 c = 0; c < 4; ++c) Set(w, 19 + 3 * c, 3, d.swz[c]);
    w[1] = u32(d.addr);
    Set(w, 64, 16, u32(d.addr >> 32));
    Set(w, 85, 3, d.block_linear ? 3 : 2);              // version de cabecera: 3 block linear, 2 pitch
    if (d.block_linear) Set(w, 99, 3, d.block_height_log2);
    else Set(w, 96, 16, d.pitch >> 5);
    Set(w, 124, 4, d.levels - 1);
    Set(w, 128, 16, d.width - 1);
    Set(w, 151, 4, d.type);
    Set(w, 160, 16, d.height - 1);
    Set(w, 176, 14, d.depth - 1);
    Set(w, 191, 1, 1);                                  // coordenadas normalizadas
    Set(w, 228, 4, d.levels - 1);                       // vista: niveles 0..levels-1
    return w;
}

struct TscDesc {
    u32 wrap = 0;                      // 0 repetir, 1 espejo, 2 borde, 3 color de borde
    u32 filter = 1;                    // 1 punto, 2 lineal (mag y min)
    u32 mip = 1;                       // 1 sin mipmaps, 2 punto, 3 lineal
    bool compare = false;
    u32 func = 0;                      // 3 = menor o igual
};

std::array<u32, 8> MakeTsc(const TscDesc& d) {
    std::array<u32, 8> w{};
    w[0] = d.wrap | (d.wrap << 3) | (d.wrap << 6) | (u32(d.compare) << 9) | (d.func << 10);
    w[1] = d.filter | (d.filter << 4) | (d.mip << 6);
    w[2] = 0xF00u << 12;               // lod maximo 15
    return w;
}

u32 Rgba(u32 r, u32 g, u32 b, u32 a = 255) { return r | (g << 8) | (b << 16) | (a << 24); }

struct TexFixture : DrawFixture {
    // Tablas con una imagen y un sampler (indice 0 de cada una), handle en c[0] del driver
    void BindTexture(Push& p, const TicDesc& tic, const TscDesc& tsc, u32 handle_offset, u32 stage = 4) {
        const auto t = MakeTic(tic);
        const auto s = MakeTsc(tsc);
        Write(TIC, t.data(), 32);
        Write(TSC, s.data(), 32);
        const u32 handle = 0;          // imagen 0 | sampler 0 << 20
        Write(DRV + handle_offset, &handle, 4);
        p.Cmd(0x557, {Hi(TSC), Lo(TSC), 0});    // SetTexSamplerPool
        p.Cmd(0x55D, {Hi(TIC), Lo(TIC), 0});    // SetTexHeaderPool
        p.Cmd(0x982, {0});                      // SetBindlessTexture: handles en c[0]
        p.Cmd(0x8E0, {0x1000, Hi(DRV), Lo(DRV)});
        p.Cmd(0x904 + 8 * stage, {1u});         // c[0] de la etapa
    }
    // Cuadrado que cubre la pantalla; color = (u, v, z) con u, v de 0 a 'span'
    void Quad(Push& p, float span, float z) {
        const V q[4] = {{-1, 1, 0, 0, 0, z}, {1, 1, 0, span, 0, z}, {-1, -1, 0, 0, span, z}, {1, -1, 0, span, span, z}};
        Write(VB, q, sizeof(q));
        DrawArrays(p, 5, 0, 4);   // tira de triangulos
    }
    // Cuadrado con el mismo color en todos los vertices
    void Flat(Push& p, float r, float g, float b) {
        const V q[4] = {{-1, 1, 0, r, g, b}, {1, 1, 0, r, g, b}, {-1, -1, 0, r, g, b}, {1, -1, 0, r, g, b}};
        Write(VB, q, sizeof(q));
        DrawArrays(p, 5, 0, 4);
    }
    // Texeles RGBA8 en block linear (bloques de 2^bh GOBs de alto)
    void WriteBlockLinear(u64 addr, u32 w, u32 h, u32 bh, const std::vector<u32>& texels) {
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x)
                Write(addr + GPU::BlockLinearOffset(x * 4, y, w * 4, bh), &texels[y * w + x], 4);
    }
};

bool Is(const std::array<u8, 4>& p, int r, int g, int b, int a, int tol = 1) {
    return Near(p[0], r, tol) && Near(p[1], g, tol) && Near(p[2], b, tol) && Near(p[3], a, tol);
}
} // namespace

TEST(Texture_Nearest2DBlockLinear) {
    // 8x8 texeles, cada uno de un color distinto; la pantalla de 64x64 muestra cada texel
    // como un cuadro de 8x8 pixeles
    TexFixture f;
    std::vector<u32> tx(64);
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) tx[y * 8 + x] = Rgba(x * 32, y * 32, 255 - x * 16, 200 + y);
    f.WriteBlockLinear(TEXMEM, 8, 8, 0, tx);
    TicDesc tic;
    tic.width = tic.height = 8;
    Push p;
    f.Setup(p, Tests::k_tri_vert, Tests::k_tex_frag);
    f.BindTexture(p, tic, TscDesc{}, 0x690);
    f.Quad(p, 1.0f, 0);
    f.Run(p);
    CHECK_EQ(f.gpu.GetStats().shader_errors, 0u);
    u32 bad = 0;
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            const auto px = f.Pixel(x, y);
            const u32 tx_x = x / 8, tx_y = y / 8;
            if (!Is(px, int(tx_x * 32), int(tx_y * 32), int(255 - tx_x * 16), int(200 + tx_y), 0)) ++bad;
        }
    CHECK_EQ(bad, 0u);
}

TEST(Texture_BilinearPitchAndWrapModes) {
    // 2x2 texeles en memoria lineal (pitch 32): rojo, verde / azul, blanco
    for (u32 wrap : {2u, 0u}) {
        TexFixture f;
        const u32 row0[2] = {Rgba(255, 0, 0), Rgba(0, 255, 0)}, row1[2] = {Rgba(0, 0, 255), Rgba(255, 255, 255)};
        f.Write(TEXMEM, row0, 8);
        f.Write(TEXMEM + 32, row1, 8);
        TicDesc tic;
        tic.width = tic.height = 2;
        tic.block_linear = false;
        tic.pitch = 32;
        TscDesc tsc;
        tsc.filter = 2;
        tsc.wrap = wrap;
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_tex_frag);
        f.BindTexture(p, tic, tsc, 0x690);
        f.Quad(p, 1.0f, 0);
        f.Run(p);
        CHECK_EQ(f.gpu.GetStats().shader_errors, 0u);
        // Centro: los 4 texeles casi por igual (pesos 0.516 / 0.484)
        CHECK(Is(f.Pixel(31, 31), 128, 123, 123, 255, 2));
        if (wrap == 2) {
            // Borde: la esquina solo ve su texel
            CHECK(Is(f.Pixel(0, 0), 255, 0, 0, 255));
            CHECK(Is(f.Pixel(63, 0), 0, 255, 0, 255));
            CHECK(Is(f.Pixel(63, 63), 255, 255, 255, 255));
        } else {
            // Repetir: en la esquina se mezcla con el lado opuesto de la textura
            CHECK(Is(f.Pixel(0, 0), 128, 123, 123, 255, 2));
        }
    }
}

TEST(Texture_MipmapsLodAndGrad) {
    // 16x16 rojo (bloques de 2 GOBs), 8x8 verde, 4x4 azul. Offsets de cada nivel:
    // 0, 1024 (16 filas x 64 bytes) y 1536 (el nivel 1 ya cabe en 1 GOB)
    struct Case { float z; int r, g, b; };
    const Case cases[] = {{0, 255, 0, 0}, {1, 0, 255, 0}, {2, 0, 0, 255}, {1.4f, 0, 255, 0}, {1.6f, 0, 0, 255},
                          {7, 0, 0, 255},                         // limitado al ultimo nivel
                          {-2.0f / 16, 0, 255, 0}, {-4.0f / 16, 0, 0, 255}, {-1.0f / 16, 255, 0, 0}};   // textureGrad
    for (const Case& c : cases) {
        TexFixture f;
        f.WriteBlockLinear(TEXMEM, 16, 16, 1, std::vector<u32>(256, Rgba(255, 0, 0)));
        f.WriteBlockLinear(TEXMEM + 1024, 8, 8, 0, std::vector<u32>(64, Rgba(0, 255, 0)));
        f.WriteBlockLinear(TEXMEM + 1536, 4, 4, 0, std::vector<u32>(16, Rgba(0, 0, 255)));
        TicDesc tic;
        tic.width = tic.height = 16;
        tic.levels = 3;
        tic.block_height_log2 = 1;
        TscDesc tsc;
        tsc.mip = 2;
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_texlod_frag);
        f.BindTexture(p, tic, tsc, 0x690);
        f.Quad(p, 1.0f, c.z);
        f.Run(p);
        CHECK_EQ(f.gpu.GetStats().shader_errors, 0u);
        CHECK(Is(f.Pixel(20, 40), c.r, c.g, c.b, 255));
    }
}

TEST(Texture_TexelFetch) {
    // Nivel 0: 8x8 con (x*30, y*30, 7); nivel 1: 4x4 con (100+x, 100+y, 9). Coordenadas
    // enteras (+0.5 para que la interpolacion no quede justo por debajo)
    struct Case { float x, y, z; int r, g, b, a; };
    const Case cases[] = {{3.5f, 5.5f, 0, 90, 150, 7, 255},
                          {2.5f, 1.5f, 1.5f, 102, 101, 9, 255},
                          {3.5f, 5.5f, -1, 120, 120, 7, 255},   // texelFetchOffset(+1, -1)
                          {9.5f, 0.5f, 0, 0, 0, 0, 0}};         // fuera de la textura: 0
    for (const Case& c : cases) {
        TexFixture f;
        std::vector<u32> l0(64), l1(16);
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 8; ++x) l0[y * 8 + x] = Rgba(x * 30, y * 30, 7);
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x) l1[y * 4 + x] = Rgba(100 + x, 100 + y, 9);
        f.WriteBlockLinear(TEXMEM, 8, 8, 0, l0);
        f.WriteBlockLinear(TEXMEM + 512, 4, 4, 0, l1);
        TicDesc tic;
        tic.width = tic.height = 8;
        tic.levels = 2;
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_texfetch_frag);
        f.BindTexture(p, tic, TscDesc{}, 0x698);   // binding 2
        f.Clear(p, 0.5f, 0.5f, 0.5f, 0.5f);
        f.Flat(p, c.x, c.y, c.z);
        f.Run(p);
        CHECK_EQ(f.gpu.GetStats().shader_errors, 0u);
        CHECK(Is(f.Pixel(10, 50), c.r, c.g, c.b, c.a, 0));
    }
}

TEST(Texture_ArrayLayersAndSize) {
    // Array de 3 capas de 4x4: rojo, verde, azul. Cada capa ocupa un bloque (512 bytes)
    struct Case { float z; int r, g, b, a; };
    const Case cases[] = {{0, 255, 0, 0, 255}, {1, 0, 255, 0, 255}, {2, 0, 0, 255, 255},
                          {-1, 4, 4, 3, 1}};   // textureSize = (4, 4, 3), 1 nivel
    for (const Case& c : cases) {
        TexFixture f;
        const u32 colors[3] = {Rgba(255, 0, 0), Rgba(0, 255, 0), Rgba(0, 0, 255)};
        for (u32 l = 0; l < 3; ++l) f.WriteBlockLinear(TEXMEM + 512 * l, 4, 4, 0, std::vector<u32>(16, colors[l]));
        TicDesc tic;
        tic.width = tic.height = 4;
        tic.depth = 3;
        tic.type = 5;   // 2D array
        Push p;
        f.Setup(p, Tests::k_tri_vert, Tests::k_texarray_frag);
        f.BindTexture(p, tic, TscDesc{}, 0x690);
        f.Quad(p, 1.0f, c.z);
        f.Run(p);
        CHECK_EQ(f.gpu.GetStats().shader_errors, 0u);
        CHECK(Is(f.Pixel(33, 17), c.r, c.g, c.b, c.a, 0));
    }
}

TEST(Texture_ShadowCompare) {
    // Profundidad Z32F de 4x4: mitad izquierda 0.25, derecha 0.75. Con "menor o igual" y
    // referencia 0.5: pasa (1) solo a la derecha
    TexFixture f;
    std::vector<u32> z(16);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) z[y * 4 + x] = F(x < 2 ? 0.25f : 0.75f);
    f.WriteBlockLinear(TEXMEM, 4, 4, 0, z);
    TicDesc tic;
    tic.format = 0x2F;   // ZF32
    tic.type_r = tic.type_rest = 7;
    tic.swz[1] = tic.swz[2] = tic.swz[3] = 2;
    tic.width = tic.height = 4;
    TscDesc tsc;
    tsc.compare = true;
    tsc.func = 3;
    Push p;
    f.Setup(p, Tests::k_tri_vert, Tests::k_texshadow_frag);
    f.BindTexture(p, tic, tsc, 0x694);   // binding 1
    f.Quad(p, 1.0f, 0.5f);
    f.Run(p);
    CHECK_EQ(f.gpu.GetStats().shader_errors, 0u);
    CHECK(Is(f.Pixel(5, 30), 0, 0, 0, 0, 0));
    CHECK(Is(f.Pixel(60, 30), 255, 255, 255, 255, 0));
}

TEST(Texture_InVertexShader) {
    // El shader de vertices lee el color de una textura 1x1 (handle en c[0] de vertices + 0x10)
    TexFixture f;
    const u32 blue = Rgba(10, 20, 250);
    f.Write(TEXMEM, &blue, 4);
    TicDesc tic;
    Push p;
    f.Setup(p, Tests::k_texvert_vert, Tests::k_color_frag);
    f.BindTexture(p, tic, TscDesc{}, 0x10, 0);
    f.Quad(p, 1.0f, 0);
    f.Run(p);
    CHECK_EQ(f.gpu.GetStats().shader_errors, 0u);
    CHECK(Is(f.Pixel(0, 0), 10, 20, 250, 255));
    CHECK(Is(f.Pixel(63, 63), 10, 20, 250, 255));
}

TEST(Texture_FormatsAndBc1) {
    // Formatos leidos directamente con TextureSampler (texelFetch), sin dibujar
    TexFixture f;
    struct Case { u32 format, type; std::vector<u8> data; u32 w; float expect[4]; };
    const Case cases[] = {
        // B5G6R5: R en los bits bajos. 0xF800 = solo azul al maximo
        {0x15, 2, {0x00, 0xF8}, 1, {0, 0, 1, 1}},
        // R16G16B16A16 float: 1.0, -2.0, 0.5, 0
        {0x03, 7, {0x00, 0x3C, 0x00, 0xC0, 0x00, 0x38, 0x00, 0x00}, 1, {1, -2, 0.5f, 0}},
        // R8 snorm: 0x81 = -127 -> -1
        {0x1D, 1, {0x81}, 1, {-1, 0, 0, 1}},
        // BC1: extremos rojo (0xF800) y azul (0x001F), indices 0, 1, 2, 3 en la primera fila
        {0x24, 2, {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0}, 4, {0, 0, 0, 0}},
    };
    u32 handle_idx = 0;
    for (const Case& c : cases) {
        const u64 addr = TEXMEM + 0x1000 * handle_idx;
        f.Write(addr, c.data.data(), c.data.size());
        TicDesc tic;
        tic.addr = addr;
        tic.format = c.format;
        tic.type_r = tic.type_rest = c.type;
        tic.width = tic.height = c.w;
        const auto t = MakeTic(tic);
        f.Write(TIC + 32 * handle_idx, t.data(), 32);
        ++handle_idx;
    }
    const auto s = MakeTsc(TscDesc{});
    f.Write(TSC, s.data(), 32);
    GPU::TextureSampler sampler(f.gpu, TIC, 15, TSC, 0);
    auto fetch = [&](u32 handle, s32 x, s32 y, float out[4]) {
        GPU::TextureRequest r;
        r.fetch = true;
        r.icoords[0] = x;
        r.icoords[1] = y;
        u32 v[4];
        sampler.Sample(handle, r, v);
        for (int i = 0; i < 4; ++i) std::memcpy(&out[i], &v[i], 4);
    };
    auto near = [](const float* a, float r, float g, float b, float al) {
        return std::fabs(a[0] - r) < 0.01f && std::fabs(a[1] - g) < 0.01f && std::fabs(a[2] - b) < 0.01f &&
               std::fabs(a[3] - al) < 0.01f;
    };
    float o[4];
    for (u32 i = 0; i < 3; ++i) {
        fetch(i, 0, 0, o);
        CHECK(near(o, cases[i].expect[0], cases[i].expect[1], cases[i].expect[2], cases[i].expect[3]));
    }
    fetch(3, 0, 0, o); CHECK(near(o, 1, 0, 0, 1));
    fetch(3, 1, 0, o); CHECK(near(o, 0, 0, 1, 1));
    fetch(3, 2, 0, o); CHECK(near(o, 2.0f / 3, 0, 1.0f / 3, 1));
    fetch(3, 3, 0, o); CHECK(near(o, 1.0f / 3, 0, 2.0f / 3, 1));
    fetch(3, 0, 1, o); CHECK(near(o, 1, 0, 0, 1));   // indices de la fila 1 = 0
    // textureSize del BC1: 4x4, 1 capa, 1 nivel
    u32 q[4];
    sampler.Query(3, 0, q);
    CHECK_EQ(q[0], 4u);
    CHECK_EQ(q[1], 4u);
    CHECK_EQ(q[3], 1u);
}

TEST(Texture_DescriptorDecodeAndLevelOffsets) {
    TicDesc d;
    d.addr = 0x1'2345'6600ull;
    d.width = 300;
    d.height = 200;
    d.levels = 5;
    d.block_height_log2 = 4;
    const auto w = MakeTic(d);
    const GPU::TextureInfo t = GPU::DecodeTic(w.data());
    CHECK(t.valid);
    CHECK_EQ(t.address, 0x1'2345'6600ull);
    CHECK_EQ(t.width, 300u);
    CHECK_EQ(t.height, 200u);
    CHECK_EQ(t.levels, 5u);
    CHECK_EQ(t.block_height_log2, 4u);
    CHECK_EQ(t.swizzle[3], 5u);
    // Nivel 0: 300*4 = 1200 bytes -> 19 GOBs de ancho; 200 filas -> 2 bloques de 128 filas
    // (16 GOBs): 19 * 2 * 8 KiB = 311296
    CHECK_EQ(GPU::TextureLevelOffset(t, 1), 19ull * 2 * 8192);
    // Nivel 1: 150x100 -> 10 GOBs de ancho; 100 filas caben en un bloque de 128: 10 * 8 KiB
    CHECK_EQ(GPU::TextureLevelOffset(t, 2) - GPU::TextureLevelOffset(t, 1), 10ull * 8192);
    // Nivel 2: 75x50 -> 300 bytes = 5 GOBs; 50 filas: el bloque baja a 8 GOBs (64 filas, 4 KiB)
    CHECK_EQ(GPU::TextureLevelOffset(t, 3) - GPU::TextureLevelOffset(t, 2), 5ull * 4096);
}
