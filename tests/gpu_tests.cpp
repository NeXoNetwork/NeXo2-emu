// Tests de la GPU emulada (fase 1): memoria de la GPU, pushbuffers, borrados de
// render targets, macros (MME), copias DMA, subida inline, 2D, syncpoints y semaforos.
// Los comandos se escriben igual que los genera deko3d (cabeceras de metodo de Maxwell).
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
#include "hle/display.hpp"

using namespace NeXo2;
using GPU::Gpu;

namespace {
constexpr u64 CPU_BASE = 0x1000'0000;   // memoria del "programa" que ve la GPU
constexpr u64 GPU_BASE = 0x4'0000'0000; // donde la proyectamos en la GPU
constexpr u64 SIZE = 0x40000;
constexpr u64 PB = GPU_BASE;            // pushbuffer
constexpr u64 RT = GPU_BASE + 0x10000;  // render target / destinos
constexpr u64 BUF = GPU_BASE + 0x20000; // buffer lineal

enum Mode : u32 { Inc = 1, NonInc = 3, Inline = 4, IncOnce = 5 };
u32 Header(Mode mode, u32 count, u32 subch, u32 method) {
    return (method & 0x1FFF) | ((subch & 7) << 13) | ((count & 0x1FFF) << 16) | (u32(mode) << 29);
}

// Construye un pushbuffer (como CmdBufWriter de deko3d)
struct Push {
    std::vector<u32> w;
    void Cmd(u32 subch, u32 method, std::initializer_list<u32> args) {
        w.push_back(Header(Inc, u32(args.size()), subch, method));
        w.insert(w.end(), args);
    }
    void Upload(u32 subch, u32 method, const std::vector<u32>& args) {   // IncreaseOnce
        w.push_back(Header(IncOnce, u32(args.size()), subch, method));
        w.insert(w.end(), args.begin(), args.end());
    }
    void Pipe(u32 subch, u32 method, const std::vector<u32>& args) {     // NonIncreasing
        w.push_back(Header(NonInc, u32(args.size()), subch, method));
        w.insert(w.end(), args.begin(), args.end());
    }
    void Imm(u32 subch, u32 method, u32 value) { w.push_back(Header(Inline, value, subch, method)); }
    void Bind(u32 subch, u32 cls) { Pipe(subch, 0, {cls}); }
};
u32 F(float f) { u32 x; std::memcpy(&x, &f, 4); return x; }
u32 Hi(u64 v) { return u32(v >> 32); }
u32 Lo(u64 v) { return u32(v); }

struct Fixture {
    Core::Memory mem;
    Gpu gpu{mem};
    GPU::Channel& ch;
    Fixture() : ch(gpu.CreateChannel()) {
        mem.WriteBytes(CPU_BASE, std::vector<u8>(SIZE, 0).data(), SIZE);
        gpu.MemoryManager().Map(GPU_BASE, CPU_BASE, SIZE);
    }
    void Run(const Push& p) {
        gpu.MemoryManager().WriteBlock(PB, p.w.data(), p.w.size() * 4);
        ch.ProcessPushbuffer(PB, u32(p.w.size()));
    }
    u32 Read32(u64 va) { return gpu.MemoryManager().Read<u32>(va); }
};

// Render target 0 (como ColorTargetBindCmds de deko3d)
void BindRt(Push& p, u64 addr, u32 width, u32 height, u32 format, bool linear, u32 pitch, u32 bh) {
    const u32 tile = linear ? (1u << 12) : (bh << 4);
    p.Cmd(0, 0x200, {Hi(addr), Lo(addr), linear ? pitch : width, height, format, tile, 1, 0});
    p.Cmd(0, 0x487, {1u | (076543210u << 4)});
    p.Cmd(0, 0x3FD, {width << 16, height << 16});
}
void ClearColor(Push& p, float r, float g, float b, float a, u32 mask = 0xF) {
    p.Cmd(0, 0x360, {F(r), F(g), F(b), F(a)});
    p.Cmd(0, 0x674, {mask << 2});
}
} // namespace

TEST(Gpu_BlockLinearMatchesDisplay) {
    const u32 pitch = 256, height = 40, bh = 2;
    std::vector<u8> linear(pitch * 64), swz(pitch * 64, 0), out(pitch * 64, 0);
    for (size_t i = 0; i < linear.size(); ++i) linear[i] = u8(i * 13 + (i >> 9));
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < pitch; ++x) swz[GPU::BlockLinearOffset(x, y, pitch, bh)] = linear[y * pitch + x];
    HLE::DeswizzleBlockLinear(swz.data(), out.data(), pitch, height, bh);
    bool same = true;
    for (u32 y = 0; y < height; ++y) same &= std::memcmp(&out[y * pitch], &linear[y * pitch], pitch) == 0;
    CHECK(same);
}

TEST(Gpu_MemoryManager) {
    Core::Memory mem;
    Gpu gpu{mem};
    auto& mm = gpu.MemoryManager();
    const u64 a = mm.Allocate(0x3000, 0x1000, false);
    const u64 b = mm.Allocate(0x20000, 0x10000, true);
    CHECK_EQ(a, GPU::GpuMemoryManager::SMALL_REGION_START);
    CHECK_EQ(b, GPU::GpuMemoryManager::BIG_REGION_START);
    CHECK_EQ(mm.Allocate(0x1000, 0x1000, false), a + 0x3000);   // la siguiente, detras
    // Dos proyecciones seguidas a sitios distintos de la CPU: una lectura las cruza
    mm.Map(b, 0x2000'0000, 0x10000);
    mm.Map(b + 0x10000, 0x3000'0000, 0x10000);
    mem.Write<u32>(0x2000'FFFC, 0x11223344);
    mem.Write<u32>(0x3000'0000, 0x55667788);
    CHECK_EQ(mm.Read<u64>(b + 0xFFFC), 0x5566778811223344ull);
    mm.Write<u64>(b + 0xFFFC, 0xAABBCCDDEEFF0011ull);
    CHECK_EQ(mem.Read<u32>(0x3000'0000), 0xAABBCCDD);
    CHECK(mm.Translate(b + 0x10010).value_or(0) == 0x3000'0010);
    CHECK_EQ(mm.Unmap(b), 0x10000);
    CHECK(!mm.Translate(b + 4).has_value());
    CHECK_EQ(mm.Read<u32>(b + 4), 0);   // sin proyectar: ceros
}

TEST(Gpu_ClearRenderTarget) {
    // Block linear RGBA8, 64x32, bloques de 4 GOBs de alto
    Fixture f;
    Push p;
    p.Bind(0, 0xB197);
    BindRt(p, RT, 64, 32, 0xD5, false, 0, 2);
    ClearColor(p, 0.0f, 0.5f, 1.0f, 1.0f);
    // Segundo borrado solo en un rectangulo (scissor 0) y solo R y A
    p.Cmd(0, 0x43E, {0x100});                         // ClearBufferFlags: usar scissor
    p.Cmd(0, 0x380, {1, 16u | (32u << 16), 8u | (16u << 16)});
    ClearColor(p, 1.0f, 0.0f, 0.0f, 0.25f, 0x9);      // R y A
    f.Run(p);
    auto px = [&](u32 x, u32 y) { return f.Read32(RT + GPU::BlockLinearOffset(x * 4, y, 64 * 4, 2)); };
    CHECK_EQ(px(0, 0), 0xFFFF8000u);                  // A=FF B=FF G=80 R=00
    CHECK_EQ(px(63, 31), 0xFFFF8000u);
    CHECK_EQ(px(16, 8), 0x40FF80FFu);                 // R=FF, A=0x40, G y B se quedan
    CHECK_EQ(px(31, 15), 0x40FF80FFu);
    CHECK_EQ(px(32, 8), 0xFFFF8000u);                 // fuera del scissor
    CHECK_EQ(f.gpu.GetStats().clears, 2);

    // Lineal BGRA8 con pitch mayor que el ancho
    Push q;
    BindRt(q, BUF, 10, 4, 0xCF, true, 64, 0);
    q.Cmd(0, 0x43E, {0});
    ClearColor(q, 1.0f, 0.0f, 0.0f, 1.0f);
    f.Run(q);
    CHECK_EQ(f.Read32(BUF), 0xFFFF0000u);             // B,G,R,A en memoria = 00 00 FF FF
    CHECK_EQ(f.Read32(BUF + 3 * 64 + 9 * 4), 0xFFFF0000u);
    CHECK_EQ(f.Read32(BUF + 3 * 64 + 10 * 4), 0u);    // detras del ancho: sin tocar
}

TEST(Gpu_ClearDepthStencil) {
    Fixture f;
    Push p;
    p.Bind(0, 0xB197);
    p.Cmd(0, 0x3F8, {Hi(RT), Lo(RT), 0x14 /* S8Z24 */, 0, 0});
    p.Cmd(0, 0x48A, {16, 8, 1});
    p.Cmd(0, 0x54E, {1});                             // DepthTargetEnable
    p.Cmd(0, 0x3FD, {16u << 16, 8u << 16});
    p.Cmd(0, 0x364, {F(1.0f)});
    p.Cmd(0, 0x368, {0x5A});
    p.Cmd(0, 0x674, {0x3});                           // profundidad + stencil
    f.Run(p);
    CHECK_EQ(f.Read32(RT + GPU::BlockLinearOffset(15 * 4, 7, 16 * 4, 0)), 0x5AFFFFFFu);
}

TEST(Gpu_MacroFillRegisters) {
    // Macro "FillRegisters" de deko3d (utils.mme): escribe un valor N veces en registros seguidos
    //   r1 to addr; fetch r2   |  fetch r3  |  .loop: dec r2  |  bnz r2 .loop  |  *r3 to mem  |  nop
    auto ins = [](u32 op, u32 result, u32 dst, u32 ra, u32 rb, s32 imm_or_alu, bool exit) {
        u32 v = op | (result << 4) | (exit ? 0x80u : 0u) | (dst << 8) | (ra << 11);
        if (op == 1 || op == 7) v |= u32(imm_or_alu) << 14;            // inmediato
        else v |= (rb << 14) | (u32(imm_or_alu) << 17);                // ALU
        return v;
    };
    const std::vector<u32> macro = {
        ins(0, 5, 2, 1, 0, 0, false),   // add r1+r0 -> metodo; r2 = parametro
        ins(0, 0, 3, 0, 0, 0, false),   // r3 = parametro
        ins(1, 1, 2, 2, 0, -1, false),  // r2 = r2 - 1
        ins(7, 1, 0, 2, 0, -1, false),  // si r2 != 0 saltar a la anterior (con hueco)
        ins(0, 4, 0, 3, 0, 0, true),    // enviar r3 (y salir si no se salto)
        0,                              // nop (hueco de la salida)
    };
    Fixture f;
    Push p;
    p.Bind(0, 0xB197);
    p.Cmd(0, 0x45, {0});
    p.Pipe(0, 0x46, macro);              // subir el codigo
    p.Cmd(0, 0x47, {3});
    p.Pipe(0, 0x48, {0, 0, 0, 0});       // la macro 3 empieza en 0
    // Llamar a la macro 3 (metodo 0xE06) como deko3d: IncreaseOnce con 3 parametros
    p.Upload(0, 0xE06, {0x360u | (1u << 12), 4, F(0.5f)});
    p.Cmd(0, 0x7F0, {0xABCD});           // otro metodo despues: no debe tocarse por la macro
    BindRt(p, RT, 16, 8, 0xD5, false, 0, 0);
    p.Cmd(0, 0x674, {0xF << 2});         // borrar con el color que puso la macro
    f.Run(p);
    for (u32 i = 0; i < 4; ++i) CHECK_EQ(f.ch.Get3D().Reg(0x360 + i), F(0.5f));
    CHECK_EQ(f.ch.Get3D().Reg(0x364), 0u);   // solo 4 escrituras
    CHECK_EQ(f.Read32(RT), 0x80808080u);
    CHECK_EQ(f.gpu.GetStats().macros, 1);
}

TEST(Gpu_DmaCopyAndFill) {
    Fixture f;
    // Rellenar el buffer lineal de origen con un patron
    std::vector<u8> pattern(64 * 16);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = u8(i * 7 + 3);
    f.gpu.MemoryManager().WriteBlock(BUF, pattern.data(), pattern.size());

    Push p;
    p.Bind(4, 0xB0B5);
    // 1) lineal (pitch 64) -> block linear (64 bytes de ancho, 16 filas, bloques de 2 GOBs)
    p.Cmd(4, 0x100, {Hi(BUF), Lo(BUF), Hi(RT), Lo(RT), 64, 64, 64, 16});
    p.Cmd(4, 0x1C3, {(1u << 4) | (1u << 12), 64, 16, 1, 0, 0});
    p.Cmd(4, 0xC0, {2u | (1u << 7) | (1u << 9)});           // origen pitch, destino block linear, varias lineas
    // 2) y de vuelta a otro buffer lineal (BUF + 0x2000), con semaforo de una palabra al terminar
    p.Cmd(4, 0x90, {Hi(BUF + 0x3000), Lo(BUF + 0x3000), 0x1234});
    p.Cmd(4, 0x100, {Hi(RT), Lo(RT), Hi(BUF + 0x2000), Lo(BUF + 0x2000), 64, 64, 64, 16});
    p.Cmd(4, 0x1CA, {(1u << 4) | (1u << 12), 64, 16, 1, 0, 0});
    p.Cmd(4, 0xC0, {2u | (1u << 8) | (1u << 9) | (1u << 3)});
    // 3) relleno ("memset") con remap: 32 elementos de 4 bytes = constante
    p.Cmd(4, 0x1C0, {0xDEADBEEF, 0});
    p.Cmd(4, 0x1C2, {4u | (3u << 16) | (0u << 24)});         // X = constante A, 4 bytes, 1 componente
    p.Cmd(4, 0x102, {Hi(BUF + 0x3100), Lo(BUF + 0x3100)});
    p.Cmd(4, 0x106, {32, 1});
    p.Cmd(4, 0xC0, {2u | (1u << 7) | (1u << 8) | (1u << 10)});
    f.Run(p);

    std::vector<u8> back(pattern.size());
    f.gpu.MemoryManager().ReadBlock(BUF + 0x2000, back.data(), back.size());
    CHECK(back == pattern);
    // El block linear no es una copia tal cual (las filas estan intercaladas)
    std::vector<u8> swz(pattern.size());
    f.gpu.MemoryManager().ReadBlock(RT, swz.data(), swz.size());
    CHECK(swz != pattern);
    CHECK_EQ(swz[GPU::BlockLinearOffset(5, 9, 64, 1)], pattern[9 * 64 + 5]);
    CHECK_EQ(f.Read32(BUF + 0x3000), 0x1234u);
    CHECK_EQ(f.Read32(BUF + 0x3100), 0xDEADBEEFu);
    CHECK_EQ(f.Read32(BUF + 0x3100 + 31 * 4), 0xDEADBEEFu);
    CHECK_EQ(f.Read32(BUF + 0x3100 + 32 * 4), 0u);
}

TEST(Gpu_InlineUploadAnd2D) {
    Fixture f;
    Push p;
    p.Bind(2, 0xA140);
    // Subir 3 palabras (12 bytes) a BUF con el motor inline
    p.Cmd(2, 0x60, {12, 1, Hi(BUF), Lo(BUF)});
    p.Cmd(2, 0x6C, {1});                      // destino lineal
    p.Pipe(2, 0x6D, {0x11111111, 0x22222222, 0x33333333});
    // 2D: copiar 2x2 pixeles RGBA8 de BUF+0x100 (pitch 16) a BUF+0x200 (pitch 32) ampliando x2
    const u32 src[4] = {0xAA0000FF, 0xBB00FF00, 0xCCFF0000, 0xDDFFFFFF};
    p.Bind(3, 0x902D);
    p.Cmd(3, 0x80, {0xD5, 1, 0, 1, 0, 32, 8, 4, Hi(BUF + 0x200), Lo(BUF + 0x200)});
    p.Cmd(3, 0x8C, {0xD5, 1, 0, 1});
    p.Cmd(3, 0x91, {16, 2, 2, Hi(BUF + 0x100), Lo(BUF + 0x100)});
    p.Cmd(3, 0x22C, {0, 0, 4, 4, 0x80000000u, 0, 0x80000000u, 0, 0, 0, 0, 0});   // du = dv = 0.5
    f.gpu.MemoryManager().WriteBlock(BUF + 0x100, &src[0], 8);
    f.gpu.MemoryManager().WriteBlock(BUF + 0x110, &src[2], 8);
    f.Run(p);
    CHECK_EQ(f.Read32(BUF), 0x11111111u);
    CHECK_EQ(f.Read32(BUF + 8), 0x33333333u);
    CHECK_EQ(f.Read32(BUF + 0x200), src[0]);
    CHECK_EQ(f.Read32(BUF + 0x204), src[0]);           // ampliado: el mismo pixel dos veces
    CHECK_EQ(f.Read32(BUF + 0x208), src[1]);
    CHECK_EQ(f.Read32(BUF + 0x200 + 2 * 32), src[2]);
    CHECK_EQ(f.Read32(BUF + 0x200 + 3 * 32 + 12), src[3]);
}

TEST(Gpu_SyncpointsAndSemaphores) {
    Fixture f;
    const u32 sp = f.ch.SyncpointId();
    bool fired = false;
    f.gpu.GetSyncpoints().AddWaiter(sp, 3, [&] { fired = true; });
    Push p;
    p.Bind(0, 0xB197);
    p.Cmd(0, 0xB2, {sp | (1u << 20)});                 // SyncptAction: +1 (como signalFence de deko3d)
    p.Cmd(0, 0xB2, {sp | (1u << 20) | (1u << 16)});
    // Semaforo del 3D (una palabra) y del canal (GPFIFO, 16 bytes)
    p.Cmd(0, 0x6C0, {Hi(BUF), Lo(BUF), 0xCAFE, 0x10000000u});
    p.Cmd(0, 0x04, {Hi(BUF + 0x10), Lo(BUF + 0x10), 0xBEEF, 2});
    p.Cmd(0, 0x1C, {0, 1u | (sp << 8)});               // syncpoint +1 desde el canal
    f.Run(p);
    CHECK_EQ(f.gpu.GetSyncpoints().Read(sp), 3u);
    CHECK(fired);
    CHECK_EQ(f.Read32(BUF), 0xCAFEu);
    CHECK_EQ(f.Read32(BUF + 0x10), 0xBEEFu);
    // Un dibujo no hace nada (fase 2) pero se cuenta
    Push d;
    d.Cmd(0, 0x35D, {0, 3});
    f.Run(d);
    CHECK_EQ(f.gpu.GetStats().draws_skipped, 1);
}

// Programa de prueba completo: abre los dispositivos de nvdrv como libnx/deko3d,
// envia comandos por un canal y comprueba el resultado (tests/programs/nro_gpu)
TEST(Gpu_NroThroughNvdrv) {
    std::ifstream fin(std::string(NEXO2_TEST_DATA_DIR) + "/gpu.nro", std::ios::binary);
    std::vector<u8> nro((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    CHECK(!nro.empty());
    if (nro.empty()) return;
    Core::System sys;
    CHECK(sys.LoadNro(nro, "gpu.nro"));
    sys.Run(50'000'000);
    const std::string out = sys.GetKernel().GetDebugOutput();
    CHECK(out.find("gpu completo, fallos: 0") != std::string::npos);
    if (out.find("fallos: 0") == std::string::npos) std::printf("%s\n", out.c_str());
    CHECK(sys.GetKernel().GetGpu().GetStats().clears >= 2);
    CHECK(sys.GetKernel().GetGpu().GetStats().macros >= 1);
}

TEST(Gpu_MacroAluAndBitfields) {
    auto alu = [](u32 result, u32 dst, u32 ra, u32 rb, u32 aluop, bool exit = false) {
        return 0u | (result << 4) | (exit ? 0x80u : 0u) | (dst << 8) | (ra << 11) | (rb << 14) | (aluop << 17);
    };
    auto addi = [](u32 result, u32 dst, u32 ra, s32 imm) { return 1u | (result << 4) | (dst << 8) | (ra << 11) | (u32(imm) << 14); };
    auto bf = [](u32 op, u32 result, u32 dst, u32 ra, u32 rb, u32 src_bit, u32 size, u32 dst_bit) {
        return op | (result << 4) | (dst << 8) | (ra << 11) | (rb << 14) | (src_bit << 17) | (size << 22) | (dst_bit << 27);
    };
    auto rd = [](u32 result, u32 dst, u32 ra, s32 imm) { return 5u | (result << 4) | (dst << 8) | (ra << 11) | (u32(imm) << 14); };
    const std::vector<u32> macro = {
        alu(0, 2, 0, 0, 0),                  // r2 = parametro 2 (5)
        bf(2, 1, 3, 0, 1, 8, 8, 4),          // r3 = insertar ((r1 >> 8) & 0xFF) en el bit 4 de r0
        bf(3, 1, 4, 1, 2, 0, 4, 8),          // r4 = ((r1 >> r2) & 0xF) << 8
        bf(4, 1, 5, 1, 2, 4, 8, 0),          // r5 = ((r1 >> 4) & 0xFF) << r2
        addi(2, 0, 0, 0x360 | (1 << 12)),    // metodo = 0x360, +1 cada vez
        alu(4, 0, 3, 0, 0),                  // enviar r3
        alu(4, 0, 4, 0, 0),                  // enviar r4
        alu(4, 0, 5, 0, 0),                  // enviar r5
        rd(4, 0, 0, 0x361),                  // enviar el registro 0x361 del motor
        alu(4, 0, 3, 4, 2),                  // enviar r3 - r4 (negativo: deja "borrow")
        alu(4, 0, 0, 0, 3),                  // enviar 0 - 0 - borrow = -1
        alu(4, 0, 1, 4, 11, true),           // enviar r1 & ~r4 y salir
        0,
    };
    Fixture f;
    Push p;
    p.Bind(0, 0xB197);
    p.Cmd(0, 0x45, {0});
    p.Pipe(0, 0x46, macro);
    p.Cmd(0, 0x47, {0});
    p.Pipe(0, 0x48, {0});
    p.Upload(0, 0xE00, {0x00ABCDEF, 5});
    f.Run(p);
    auto& r = f.ch.Get3D();
    CHECK_EQ(r.Reg(0x360), 0xCD0u);
    CHECK_EQ(r.Reg(0x361), 0xF00u);                // (0xABCDEF >> 5) & 0xF = 0xF
    CHECK_EQ(r.Reg(0x362), 0xDEu << 5);
    CHECK_EQ(r.Reg(0x363), 0xF00u);
    CHECK_EQ(r.Reg(0x364), 0xCD0u - 0xF00u);
    CHECK_EQ(r.Reg(0x365), 0xFFFFFFFFu);
    CHECK_EQ(r.Reg(0x366), 0xABC0EFu);
    CHECK_EQ(r.Reg(0x367), 0u);
}

TEST(Gpu_DmaRemapSwapComponents) {
    Fixture f;
    const u16 src[8] = {0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888};
    f.gpu.MemoryManager().WriteBlock(BUF, src, sizeof(src));
    Push p;
    p.Bind(4, 0xB0B5);
    // 4 elementos de 2 componentes de 2 bytes: destino X = origen Y, destino Y = origen X
    p.Cmd(4, 0x1C2, {1u | (0u << 4) | (1u << 16) | (1u << 20) | (1u << 24)});
    p.Cmd(4, 0x100, {Hi(BUF), Lo(BUF), Hi(BUF + 0x100), Lo(BUF + 0x100)});
    p.Cmd(4, 0x106, {4, 1});
    p.Cmd(4, 0xC0, {2u | (1u << 7) | (1u << 8) | (1u << 10)});
    f.Run(p);
    u16 out[8];
    f.gpu.MemoryManager().ReadBlock(BUF + 0x100, out, sizeof(out));
    CHECK_EQ(out[0], 0x2222u);
    CHECK_EQ(out[1], 0x1111u);
    CHECK_EQ(out[6], 0x8888u);
    CHECK_EQ(out[7], 0x7777u);
}

// Las macros reales de deko3d (ensambladas con su herramienta): asi el interprete de
// macros se comprueba con el mismo codigo que ejecutaria un homebrew con deko3d.
#include "generated/deko3d_macros.hpp"

TEST(Gpu_Deko3dRealMacros) {
    Fixture f;
    Push p;
    p.Bind(0, 0xB197);
    p.w.insert(p.w.end(), std::begin(MmeMacro_SetupCmds), std::end(MmeMacro_SetupCmds));   // Queue::setupEngines
    // Render target de 3 capas (16x8 RGBA8, block linear de 1 GOB de alto: cada capa 0x200 bytes)
    p.Cmd(0, 0x200, {Hi(RT), Lo(RT), 16, 8, 0xD5, 0, 3, 0x200 / 4});
    p.Cmd(0, 0x487, {1u | (076543210u << 4)});
    p.Cmd(0, 0x3FD, {16u << 16, 8u << 16});
    p.Cmd(0, 0x360, {F(1.0f), F(1.0f), F(0.0f), F(1.0f)});
    p.Imm(0, MmeMacroClearColor, 0xF << 2);                    // dkCmdBufClearColor: MacroInline(ClearColor, ...)
    // BindColorBlendEnableState(0b10100101) y FillRegisters
    p.Imm(0, MmeMacroBindColorBlendEnableState, 0xA5);
    p.Upload(0, MmeMacroFillRegisters, {0x380u | (4u << 12), 3, 7});   // Scissor[0..2].Enable = 7
    f.Run(p);
    for (u32 layer = 0; layer < 3; ++layer)
        CHECK_EQ(f.Read32(RT + layer * 0x200 + GPU::BlockLinearOffset(15 * 4, 7, 64, 0)), 0xFF00FFFFu);
    CHECK_EQ(f.Read32(RT + 3 * 0x200), 0u);                     // la cuarta "capa" no existe
    CHECK_EQ(f.gpu.GetStats().clears, 3);
    auto& r = f.ch.Get3D();
    for (u32 i = 0; i < 8; ++i) CHECK_EQ(r.Reg(0x4D8 + i), (0xA5u >> i) & 1);
    CHECK_EQ(r.Reg(0x380), 7u);
    CHECK_EQ(r.Reg(0x384), 7u);
    CHECK_EQ(r.Reg(0x388), 7u);
    CHECK_EQ(r.Reg(0x38C), 0u);
}

TEST(Gpu_ShadowRamFirmwareCallAndMsaaClear) {
    Fixture f;
    Push p;
    p.Bind(0, 0xB197);
    // MmeShadowRamControl: track (guarda), passthrough (no guarda), replay (usa lo guardado)
    p.Cmd(0, 0x49, {0});
    p.Cmd(0, 0x3E0, {5});
    p.Cmd(0, 0x49, {2});
    p.Cmd(0, 0x3E0, {7});
    f.Run(p);
    auto& r = f.ch.Get3D();
    CHECK_EQ(r.Reg(0x3E0), 7u);
    Push p2;
    p2.Cmd(0, 0x49, {3});
    p2.Cmd(0, 0x3E0, {99});
    p2.Cmd(0, 0x49, {1});
    // Macro WriteHardwareReg de deko3d (tal cual la sube deko_examples.nro): pasa los
    // argumentos en MmeFirmwareArgs, llama a FirmwareCall[4] y espera a que el firmware
    // ponga MmeFirmwareArgs[0] = 1
    const std::vector<u32> whr = {0x00110071, 0x07400251, 0x00000331, 0x00001041, 0x00001841, 0x02310021,
                                  0x00000841, 0x03400115, 0xFFFFC911, 0xFFFF8817, 0x001000F1, 0x00000011};
    p2.Cmd(0, 0x45, {0});
    p2.Pipe(0, 0x46, whr);
    p2.Cmd(0, 0x47, {0});
    p2.Pipe(0, 0x48, {0});
    p2.Upload(0, 0xE00, {0x418800, 1, 1});
    // MSAA 2x2: render target de 32x16 muestras = 16x8 pixeles; el scissor va en pixeles
    p2.Cmd(0, 0x574, {2});
    p2.Cmd(0, 0x200, {Hi(RT), Lo(RT), 32, 16, 0xD5, 0, 1, 0});
    p2.Cmd(0, 0x487, {1u | (076543210u << 4)});
    p2.Cmd(0, 0x3FD, {16u << 16, 8u << 16});
    p2.Cmd(0, 0x360, {F(1.0f), F(0.0f), F(0.0f), F(1.0f)});
    p2.Cmd(0, 0x674, {0xFu << 2});
    f.Run(p2);
    CHECK_EQ(r.Reg(0x3E0), 5u);                     // replay devolvio el valor guardado
    CHECK_EQ(r.Reg(0x8C4), 0x418800u);              // FirmwareCall[4] con el registro de PGRAPH
    CHECK_EQ(r.Reg(0xD00), 1u);                     // "hecho": la macro no se queda en bucle
    CHECK_EQ(f.Read32(RT + GPU::BlockLinearOffset(31 * 4, 15, 32 * 4, 0)), 0xFF0000FFu);   // ultima muestra
}
