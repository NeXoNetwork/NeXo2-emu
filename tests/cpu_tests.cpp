// Tests de la CPU ARM64 de NeXo 2.
//
// Cada test carga un programa de tests/programs/ en memoria, lo ejecuta en el
// interprete hasta llegar a "brk #0" y comprueba los registros/memoria.
//
// Compilar y ejecutar: ver BUILDING.md (seccion "Run the tests").

#include <cstring>
#include <string>

#include "test_framework.hpp"
#include "arm64/interpreter.hpp"
#include "memory.hpp"
#include "generated/test_programs.hpp"

using namespace NeXo2::Core;
namespace P = NeXo2::Tests::Programs;

// ---------------------------------------------------------------------------
// Entorno de ejecucion: memoria + CPU con un programa cargado
// ---------------------------------------------------------------------------
constexpr u64 CODE_BASE   = 0x80000000;  // donde se carga el programa (igual que main.cpp)
constexpr u64 DATA_BASE   = 0x90000000;  // zona libre para datos
constexpr u64 STACK_TOP   = 0xA0000000;  // la pila crece hacia abajo desde aqui
constexpr u64 RETURN_ADDR = 0x7FFF0000;  // las funciones C "vuelven" aqui (hay un BRK)

struct Machine {
    Memory      mem;
    Interpreter cpu{mem};

    template <size_t N>
    explicit Machine(const uint32_t (&program)[N]) {
        for (size_t i = 0; i < N; ++i) mem.Write<u32>(CODE_BASE + i * 4, program[i]);
        mem.Write<u32>(RETURN_ADDR, 0xD4200000u); // brk #0
        cpu.GetState().pc = CODE_BASE;
        cpu.GetState().sp = STACK_TOP;
    }

    u64 x(unsigned n) const { return cpu.GetState().x[n]; }

    // Ejecuta hasta BRK. Falla el test si se para por otro motivo.
    void RunToBreak(u64 max_steps = 10'000'000) {
        cpu.Run(max_steps);
        CHECK(cpu.IsHalted());
        if (cpu.GetHaltReason().rfind("BRK", 0) != 0)
            std::printf("    Parada inesperada: %s\n", cpu.GetHaltReason().c_str());
        CHECK(cpu.GetHaltReason().rfind("BRK", 0) == 0);
    }

    // Llama a una funcion compilada en C (convenio de llamada AAPCS64: args en X0..X7).
    u64 Call(u64 function_offset, std::initializer_list<u64> args) {
        auto& st = cpu.GetState();
        unsigned i = 0;
        for (u64 a : args) st.x[i++] = a;
        st.x[30] = RETURN_ADDR;
        st.sp    = STACK_TOP;
        st.pc    = CODE_BASE + function_offset;
        cpu.Resume();
        RunToBreak();
        CHECK_EQ(st.pc, RETURN_ADDR);
        return st.x[0];
    }
};

// ---------------------------------------------------------------------------
// Tests con programas en ensamblador
// ---------------------------------------------------------------------------
TEST(MovWide) {
    Machine m(P::mov_wide);
    m.RunToBreak();
    CHECK_EQ(m.x(0), 0x56780000ABCD1234ull);
    CHECK_EQ(m.x(1), 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(m.x(2), 0x00000000FFFFFFFFull); // MOVN W pone a cero la parte alta
    CHECK_EQ(m.x(3), 0xFFFF0000ull);
}

TEST(AddSubAndStackPointer) {
    Machine m(P::add_sub_sp);
    m.RunToBreak();
    CHECK_EQ(m.x(0), STACK_TOP);
    CHECK_EQ(m.x(1), STACK_TOP - 0x20);
    CHECK_EQ(m.x(2), STACK_TOP - 0x20 + 0x1000);
    CHECK_EQ(m.x(3), 1);                       // 0xFFFFFFFF + 2 en 32 bits
    CHECK_EQ(m.cpu.GetState().sp, STACK_TOP);
}

TEST(LoopAndFlags) {
    Machine m(P::loop_flags);
    m.RunToBreak();
    CHECK_EQ(m.x(0), 55);
    CHECK_EQ(m.x(1), 0);
    CHECK_EQ(m.x(3), 1); // LT (con signo)
    CHECK_EQ(m.x(4), 0); // LO (sin signo)
    CHECK_EQ(m.x(5), 1); // EQ
}

TEST(CallAndReturn) {
    Machine m(P::call_ret);
    m.RunToBreak();
    CHECK_EQ(m.x(19), 36);
    CHECK_EQ(m.x(20), 49);
    CHECK_EQ(m.cpu.GetState().sp, STACK_TOP);
}

TEST(LoadStore) {
    Machine m(P::load_store);
    m.RunToBreak();
    const u64 v = 0x7788556633441122ull;
    CHECK_EQ(m.x(3), v);
    CHECK_EQ(m.x(4), 0x22);
    CHECK_EQ(m.x(5), 0x3344);
    CHECK_EQ(m.x(6), 0x77885566);
    CHECK_EQ(m.x(8), (u64)-2);           // LDRSW
    CHECK_EQ(m.x(9), (u64)-2);           // LDRSB
    CHECK_EQ(m.x(10), 0xFE);             // LDRB
    CHECK_EQ(m.x(1), DATA_BASE + 16 + 8); // pre-indice + post-indice
    CHECK_EQ(m.x(11), v);
    CHECK_EQ(m.x(13), v);                // [x1 + (-1 << 3)]
    CHECK_EQ(m.x(14), v);                // LDUR
    CHECK_EQ(m.x(15), 0x0123456789ABCDEFull); // LDR literal
    CHECK_EQ(m.x(16), 0x22);
    CHECK_EQ(m.x(17), 0x3344);
    CHECK_EQ(m.x(18), (u64)-2);          // LDPSW
    CHECK_EQ(m.x(19), 0);
}

TEST(LogicalAndBitfield) {
    Machine m(P::bitfield);
    m.RunToBreak();
    const u64 x0 = 0x80000000DEADBEEFull;
    CHECK_EQ(m.x(0), x0);
    CHECK_EQ(m.x(1), 0xEF);
    CHECK_EQ(m.x(2), 0xAAAAAAAAAAAAAAAAull);
    CHECK_EQ(m.x(3), x0 ^ 0xFFFF0000FFFF0000ull);
    CHECK_EQ(m.x(4), x0 << 4);
    CHECK_EQ(m.x(5), 8);
    CHECK_EQ(m.x(6), 0xFFFFFFFFFFFFFFF8ull);
    CHECK_EQ(m.x(7), 0xBE);
    CHECK_EQ(m.x(8), (u64)-5);
    CHECK_EQ(m.x(9), 0xEF0000);
    CHECK_EQ(m.x(10), 0xFFFFFFFFDEADBEEFull);
    CHECK_EQ(m.x(11), 0xEF);
    CHECK_EQ(m.x(12), 0xFFFFFFFFFFFFBEEFull);
    CHECK_EQ(m.x(13), (x0 >> 4) | (x0 << 60));
    CHECK_EQ(m.x(14), 1);
}

TEST(DataProcessingRegister) {
    Machine m(P::dp_reg);
    m.RunToBreak();
    CHECK_EQ(m.x(2), 14);
    CHECK_EQ(m.x(3), 2);
    CHECK_EQ(m.x(5), (u64)-14);
    CHECK_EQ(m.x(6), 0);
    CHECK_EQ(m.x(7), 700);
    CHECK_EQ(m.x(9), 0xFFFFFFFFFFFFFFFEull);
    CHECK_EQ(m.x(10), 0);
    CHECK_EQ(m.x(11), 61);
    CHECK_EQ(m.x(13), 0x02010403);
    CHECK_EQ(m.x(14), 100);
    CHECK_EQ(m.x(15), 8);
    CHECK_EQ(m.x(16), (u64)-7);
    CHECK_EQ(m.x(17), 1);
    CHECK_EQ(m.x(18), 100);
    CHECK_EQ(m.x(19), 96);
    CHECK_EQ(m.x(20), 128);
    CHECK_EQ(m.x(21), 93);
    CHECK_EQ(m.x(22), 896);
    CHECK_EQ(m.x(23), 0);
    CHECK_EQ(m.x(24), 1);
}

TEST(Branches) {
    Machine m(P::branches);
    m.RunToBreak();
    CHECK_EQ(m.x(1), 0); // ninguna instruccion "trampa" se ejecuto
    CHECK_EQ(m.x(3), 1);
}

TEST(SvcAndSystemRegisters) {
    Machine m(P::system);
    u32 svc_number = 0;
    m.cpu.SetSvcHandler([&](u32 imm, CPUState& st) {
        svc_number = imm;
        st.x[0] += 1;
    });
    m.cpu.GetState().tpidrro_el0 = 0xCAFE000;
    m.RunToBreak();
    CHECK_EQ(svc_number, 0x26);
    CHECK_EQ(m.x(0), 43);
    CHECK_EQ(m.x(1), 0xCAFE000);
    CHECK_EQ(m.x(2), 31'250'000);
    CHECK_EQ(m.x(4), 0x1234);
    CHECK_EQ(m.x(5), 0x60000000);
}

TEST(Atomics) {
    Machine m(P::atomics);
    m.RunToBreak();
    CHECK_EQ(m.x(5), 15);
    CHECK_EQ(m.x(7), 15);
    CHECK_EQ(m.x(9), 18);
    CHECK_EQ(m.x(10), 100);
    CHECK_EQ(m.x(12), 200);
    CHECK_EQ(m.x(13), 10);
}

TEST(PcRelativeAddresses) {
    Machine m(P::adr);
    m.RunToBreak();
    CHECK_EQ(m.x(0), CODE_BASE + 0xC);
    CHECK_EQ(m.x(1), CODE_BASE & ~0xFFFull);
}

TEST(UnimplementedInstructionStopsCpu) {
    const uint32_t program[] = { 0xD2800020u /* mov x0, #1 */, 0x00000000u /* udf */ };
    Machine m(program);
    m.cpu.Run(100);
    CHECK(m.cpu.IsHalted());
    CHECK_EQ(m.cpu.GetState().pc, CODE_BASE + 4);   // se queda en la instruccion que fallo
    CHECK_EQ(m.x(0), 1);
    CHECK(m.cpu.GetHaltReason().find("no implementado") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Tests con codigo C compilado por clang (tests/programs/c_functions.c)
// Se comparan los resultados con la misma funcion ejecutada en el PC.
// ---------------------------------------------------------------------------
namespace Host {
u64 fibonacci(u32 n) { u64 a = 0, b = 1; for (u32 i = 0; i < n; ++i) { u64 t = a + b; a = b; b = t; } return a; }
u64 factorial(u64 n) { return n <= 1 ? 1 : n * factorial(n - 1); }
u64 gcd(u64 a, u64 b) { while (b) { u64 t = a % b; a = b; b = t; } return a; }
u32 count_primes(u32 limit) {
    u32 c = 0;
    for (u32 n = 2; n < limit; ++n) { bool p = true; for (u32 d = 2; d * d <= n; ++d) if (n % d == 0) { p = false; break; } c += p; }
    return c;
}
u32 fnv1a(const unsigned char* d, u64 n) { u32 h = 2166136261u; for (u64 i = 0; i < n; ++i) { h ^= d[i]; h *= 16777619u; } return h; }
}

TEST(CompiledC_Fibonacci) {
    Machine m(P::c_functions);
    for (u32 n : {0u, 1u, 2u, 10u, 50u, 90u})
        CHECK_EQ(m.Call(P::c_functions_fibonacci, {n}), Host::fibonacci(n));
}

TEST(CompiledC_RecursiveFactorial) {
    Machine m(P::c_functions);
    CHECK_EQ(m.Call(P::c_functions_factorial_rec, {20}), Host::factorial(20));
    CHECK_EQ(m.cpu.GetState().sp, STACK_TOP); // la pila queda equilibrada
}

TEST(CompiledC_StringLength) {
    Machine m(P::c_functions);
    const char text[] = "Hola NeXo 2!";
    m.mem.WriteBytes(DATA_BASE, text, sizeof(text));
    CHECK_EQ(m.Call(P::c_functions_string_length, {DATA_BASE}), std::strlen(text));
}

TEST(CompiledC_BubbleSort) {
    Machine m(P::c_functions);
    const s64 input[]    = {5, -3, 9, 0, -100, 42, 7};
    const s64 expected[] = {-100, -3, 0, 5, 7, 9, 42};
    m.mem.WriteBytes(DATA_BASE, input, sizeof(input));
    m.Call(P::c_functions_bubble_sort, {DATA_BASE, 7});
    for (int i = 0; i < 7; ++i)
        CHECK_EQ(m.mem.Read<u64>(DATA_BASE + i * 8), (u64)expected[i]);
}

TEST(CompiledC_Gcd) {
    Machine m(P::c_functions);
    CHECK_EQ(m.Call(P::c_functions_gcd, {1071, 462}), Host::gcd(1071, 462));
    CHECK_EQ(m.Call(P::c_functions_gcd, {0xFFFFFFFFFFFFFFC5ull, 0x123456789ull}),
             Host::gcd(0xFFFFFFFFFFFFFFC5ull, 0x123456789ull));
}

TEST(CompiledC_CountPrimes) {
    Machine m(P::c_functions);
    CHECK_EQ(m.Call(P::c_functions_count_primes, {1000}), Host::count_primes(1000));
}

TEST(CompiledC_HashFnv1a) {
    Machine m(P::c_functions);
    const unsigned char data[] = "NeXo 2 | Switch 2 research";
    m.mem.WriteBytes(DATA_BASE, data, sizeof(data) - 1);
    CHECK_EQ(m.Call(P::c_functions_hash_fnv1a, {DATA_BASE, sizeof(data) - 1}),
             Host::fnv1a(data, sizeof(data) - 1));
}
