// Tests de la cache de instrucciones decodificadas (interpreter_fast.cpp).
//
// La idea: dos CPUs identicas, una con la cache y otra sin ella. Se ejecuta la
// MISMA instruccion (generada al azar) en las dos con el mismo estado inicial y
// se exige que el resultado sea identico: registros, flags, PC, memoria y si la
// instruccion era valida o no. Asi cualquier diferencia entre una funcion rapida
// y el codigo normal salta enseguida, con la instruccion que falla.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "system.hpp"
#include "common/logger.hpp"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;
using Core::Interpreter;
using Core::Memory;

namespace {
constexpr u64 CODE = 0x40000000;        // donde se escribe la instruccion de prueba
constexpr u64 DATA = 0x10000000;        // zona de datos para lecturas/escrituras
constexpr u64 WINDOW = 0x10000;         // tamano de cada zona que se compara

// Una familia de instrucciones: los bits de 'mask' valen 'value', el resto al azar
struct Pattern { const char* name; u32 mask; u32 value; bool memory; };

const Pattern PATTERNS[] = {
    {"add/sub #imm",        0x1F800000, 0x11000000, false},
    {"logico #imm",         0x1F800000, 0x12000000, false},
    {"movn/movz/movk",      0x1F800000, 0x12800000, false},
    {"bitfield",            0x1F800000, 0x13000000, false},
    {"lsl/lsr/asr #",       0x7F800000, 0x53000000, false},
    {"logico registro",     0x1F000000, 0x0A000000, false},
    {"mov registro",        0x7FE0FFE0, 0x2A0003E0, false},
    {"add/sub registro",    0x1F200000, 0x0B000000, false},
    {"csel",                0x1FE00000, 0x1A800000, false},
    {"lslv/lsrv/asrv/rorv", 0x7FE0F000, 0x1AC02000, false},
    {"madd/msub",           0x7FE00000, 0x1B000000, false},
    {"b / bl",              0x7C000000, 0x14000000, false},
    {"b.cond",              0xFF000010, 0x54000000, false},
    {"cbz/cbnz",            0x7E000000, 0x34000000, false},
    {"tbz/tbnz",            0x7E000000, 0x36000000, false},
    {"br/blr/ret",          0xFF9FFC1F, 0xD61F0000, false},
    {"ldr/str #imm12",      0x3F000000, 0x39000000, true},
    {"ldr/str imm9",        0x3F200000, 0x38000000, true},
    {"ldr/str registro",    0x3F200C00, 0x38200800, true},
    {"ldp/stp",             0x3E000000, 0x28000000, true},
    // Todo el grupo al azar: comprueba que Decode() no elige mal la funcion
    {"grupo dp inmediato",  0x1C000000, 0x10000000, false},
    {"grupo dp registro",   0x0E000000, 0x0A000000, false},
    {"grupo saltos",        0x1C000000, 0x14000000, false},
    {"grupo memoria",       0x0A000000, 0x08000000, true},
};

struct Cpu {
    Memory mem;
    Interpreter cpu{mem};
};

u64 RandomValue(std::mt19937_64& rng, bool memory) {
    switch (rng() % (memory ? 4 : 6)) {
        case 0:  return rng() % 0x100;                                   // pequeno (indices)
        case 1:  return DATA + 0x4000 + (rng() % 0x800) - 0x400;         // direccion de datos
        case 2:  return 0x4000 + (rng() % 0x800) - 0x400;                // direccion baja
        case 3:  { const u64 s[] = {0, 1, ~0ull, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                                    0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull};
                   return s[rng() % 8]; }
        default: return rng();                                           // cualquiera
    }
}

// Compara una zona de memoria pagina a pagina (nullptr = pagina sin crear = ceros)
bool SameMemory(Memory& a, Memory& b, u64 base, u64 size) {
    static const u8 zeros[Memory::PAGE_SIZE] = {};
    for (u64 p = base; p < base + size; p += Memory::PAGE_SIZE) {
        const u8* x = a.PagePointer(p);
        const u8* y = b.PagePointer(p);
        if (std::memcmp(x ? x : zeros, y ? y : zeros, Memory::PAGE_SIZE) != 0) return false;
    }
    return true;
}

bool SameState(const Core::CPUState& a, const Core::CPUState& b) {
    return a.x == b.x && a.sp == b.sp && a.pc == b.pc &&
           a.flags.n == b.flags.n && a.flags.z == b.flags.z &&
           a.flags.c == b.flags.c && a.flags.v == b.flags.v &&
           a.tpidr_el0 == b.tpidr_el0 && a.v == b.v;
}
} // namespace

TEST(DecodeCache_SameResultAsInterpreter) {
    Common::Logger::SetMuted(true);   // miles de instrucciones al azar: muchas no existen
    std::mt19937_64 rng(12345);
    Cpu slow, fast;
    // "slow" = interprete sin cache. "fast" = cache de decodificacion o, si los tests
    // se ejecutan en modo JIT, el JIT: asi este test compara tambien JIT contra interprete.
    slow.cpu.SetJitEnabled(false);
    slow.cpu.SetDecodeCacheEnabled(false);
    fast.cpu.SetDecodeCacheEnabled(true);

    int failures = 0;
    for (const Pattern& p : PATTERNS) {
        for (int i = 0; i < 1500 && failures < 5; ++i) {
            const u32 raw = (u32(rng()) & ~p.mask) | p.value;

            // Mismo estado al azar en las dos CPUs
            Core::CPUState st{};
            st.Reset();
            for (int r = 0; r < 31; ++r) st.x[r] = RandomValue(rng, p.memory);
            st.sp = DATA + 0x4000 + (rng() % 0x400) * 16 - 0x2000;
            st.SetNZCV(u64(rng() % 16) << 28);
            st.pc = CODE;
            for (Cpu* c : {&slow, &fast}) {
                c->mem.Write<u32>(CODE, raw);   // en "fast" esto invalida la instruccion anterior
                c->cpu.GetState() = st;
                c->cpu.Resume();
            }

            const u64 ns = slow.cpu.Run(1);
            const u64 nf = fast.cpu.Run(1);
            bool same = ns == nf && slow.cpu.IsHalted() == fast.cpu.IsHalted() &&
                        SameState(slow.cpu.GetState(), fast.cpu.GetState());
            if (same && p.memory)
                same = SameMemory(slow.mem, fast.mem, DATA, WINDOW) && SameMemory(slow.mem, fast.mem, 0, WINDOW);
            if (!same) {
                ++failures;
                std::printf("    DIFERENCIA en %s: instruccion 0x%08X\n", p.name, raw);
            }
        }
    }
    Common::Logger::SetMuted(false);
    CHECK_EQ(failures, 0);
    if (!fast.cpu.IsJitEnabled()) CHECK(fast.cpu.DecodedPages() > 0);
}

TEST(DecodeCache_SelfModifyingCode) {
    // Un programa que se reescribe a si mismo: la cache tiene que notarlo.
    //   0: mov x0, #1
    //   4: str w1, [x2]      (x2 = direccion de la instruccion 0; w1 = "mov x0, #7")
    //   8: b 0               (vuelve a la instruccion 0, que ahora es otra)
    Memory mem;
    Interpreter cpu(mem);
    const u64 base = 0x80000000;
    mem.Write<u32>(base + 0, 0xD2800020);   // mov x0, #1
    mem.Write<u32>(base + 4, 0xB9000041);   // str w1, [x2]
    mem.Write<u32>(base + 8, 0x17FFFFFE);   // b -8
    cpu.GetState().pc = base;
    cpu.GetState().x[1] = 0xD28000E0;       // mov x0, #7
    cpu.GetState().x[2] = base;

    cpu.Run(3);                             // mov #1, str (reescribe), b
    CHECK_EQ(cpu.GetState().x[0], 1);
    cpu.Run(1);                             // la instruccion 0 ya es "mov x0, #7"
    CHECK_EQ(cpu.GetState().x[0], 7);

    // Escribir codigo desde fuera (como hace el cargador) tambien cuenta
    mem.Write<u32>(base + 4, 0xD2800140);   // mov x0, #10
    cpu.Run(1);
    CHECK_EQ(cpu.GetState().x[0], 10);
}

TEST(DecodeCache_RealProgramIdentical) {
    // libnx_init.nro entero con y sin cache: mismo estado final, misma salida
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/libnx_init.nro", std::ios::binary);
    std::vector<u8> nro((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Core::System a, b;
    a.GetCpu().SetJitEnabled(false);          // a = interprete sin cache; b = cache o JIT
    a.GetCpu().SetDecodeCacheEnabled(false);
    CHECK(a.LoadNro(nro, "libnx_init.nro"));
    CHECK(b.LoadNro(nro, "libnx_init.nro"));
    Common::Logger::SetMuted(true);
    const u64 na = a.Run(5'000'000);
    const u64 nb = b.Run(5'000'000);
    Common::Logger::SetMuted(false);
    CHECK_EQ(na, nb);
    CHECK(SameState(a.GetCpu().GetState(), b.GetCpu().GetState()));
    // La salida incluye la hora del PC ("hora unix=..."): si entre las dos ejecuciones
    // cambia el segundo, esa linea es distinta. Se compara todo lo demas.
    auto without_clock = [](std::string s) {
        const size_t p = s.find("hora unix=");
        if (p != std::string::npos) s.erase(p, s.find('\n', p) - p);
        return s;
    };
    CHECK(without_clock(a.GetKernel().GetDebugOutput()) == without_clock(b.GetKernel().GetDebugOutput()));
    if (!b.GetCpu().IsJitEnabled()) CHECK(b.GetCpu().DecodedPages() > 0);
}
