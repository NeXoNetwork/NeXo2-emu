// Tests "diferenciales" de SIMD y coma flotante.
//
// Cada programa de tests/programs/simd/ se ejecuto en un ARM64 de referencia (QEMU)
// y se guardaron todos sus registros en tests/generated/simd_tests.hpp
// (tools/gen_simd_tests.py). Aqui se ejecuta el mismo codigo en NeXo y se compara
// registro a registro: x0-x28, NZCV y v0-v31.

#include <cstdio>
#include "test_framework.hpp"
#include "arm64/interpreter.hpp"
#include "memory.hpp"
#include "generated/simd_tests.hpp"

using namespace NeXo2::Core;
namespace S = NeXo2::Tests::Simd;

namespace {

// Ejecuta un caso y devuelve cuantos registros no coinciden
int RunCase(const S::Case& c) {
    constexpr u64 BASE = 0x80000000, STACK = 0xA0000000;
    Memory mem;
    Interpreter cpu(mem);
    for (size_t i = 0; i < c.code_words; ++i) mem.Write<u32>(BASE + i * 4, c.code[i]);
    cpu.GetState().pc = BASE;
    cpu.GetState().sp = STACK;
    cpu.Run(100000);

    int bad = 0;
    if (cpu.GetHaltReason().rfind("BRK", 0) != 0) {
        std::printf("    [%s] parada inesperada: %s\n", c.name, cpu.GetHaltReason().c_str());
        return 1;
    }
    const auto& st = cpu.GetState();
    for (int i = 0; i < 29; ++i) {
        if (st.x[i] != c.x[i]) {
            std::printf("    [%s] x%d = %016llX, ARM real: %016llX\n", c.name, i,
                        (unsigned long long)st.x[i], (unsigned long long)c.x[i]);
            ++bad;
        }
    }
    if (st.GetNZCV() != c.nzcv) {
        std::printf("    [%s] NZCV = %08llX, ARM real: %08llX\n", c.name,
                    (unsigned long long)st.GetNZCV(), (unsigned long long)c.nzcv);
        ++bad;
    }
    for (int i = 0; i < 32; ++i) {
        if (st.v[i].lo != c.v[2 * i] || st.v[i].hi != c.v[2 * i + 1]) {
            std::printf("    [%s] v%-2d = %016llX:%016llX, ARM real: %016llX:%016llX\n", c.name, i,
                        (unsigned long long)st.v[i].hi, (unsigned long long)st.v[i].lo,
                        (unsigned long long)c.v[2 * i + 1], (unsigned long long)c.v[2 * i]);
            ++bad;
        }
    }
    return bad;
}

} // namespace

#define SIMD_TEST(name) TEST(Simd_##name) { CHECK_EQ(RunCase(S::case_##name), 0); }

SIMD_TEST(ldst)
SIMD_TEST(fp_arith)
SIMD_TEST(fp_cmp_cvt)
SIMD_TEST(copy_imm)
SIMD_TEST(int_vec)
SIMD_TEST(misc_shift)
SIMD_TEST(permute)
SIMD_TEST(fp_vec)
