#pragma once
// Base comun de los tests de dibujo: memoria, GPU, pushbuffer y el estado inicial que deja
// deko3d (render target 64x64, viewport, vertices con posicion y color).
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "memory.hpp"
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

inline u32 Hdr(u32 mode, u32 count, u32 method) { return (method & 0x1FFF) | ((count & 0x1FFF) << 16) | (mode << 29); }
inline u32 F(float f) { u32 x; std::memcpy(&x, &f, 4); return x; }
inline u32 Hi(u64 v) { return u32(v >> 32); }
inline u32 Lo(u64 v) { return u32(v); }

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
[[maybe_unused]] const V kTri[3] = {{-1, -1, 0.5f, 1, 0, 0}, {1, -1, 0.5f, 0, 1, 0}, {0, 1, 0.5f, 0, 0, 1}};

inline bool Near(int a, int b, int tol = 3) { return std::abs(a - b) <= tol; }
} // namespace
