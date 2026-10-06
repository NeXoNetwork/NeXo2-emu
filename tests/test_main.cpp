// Punto de entrada de nexo2_tests: ejecuta todos los TEST(...) registrados
// en cpu_tests.cpp, loader_tests.cpp, etc.
//
// Si NeXo se compilo con el JIT, todos los tests se ejecutan DOS veces: con el
// interprete y con el JIT (todas las CPUs que creen los tests usan el JIT).
//   nexo2_tests            -> los dos
//   nexo2_tests --interp   -> solo interprete
//   nexo2_tests --jit      -> solo JIT
#include <cstdio>
#include <cstring>
#include "test_framework.hpp"
#include "arm64/interpreter.hpp"

int main(int argc, char** argv) {
    using namespace NeXo2::Tests;
    using NeXo2::Core::Interpreter;
    bool run_interp = true, run_jit = Interpreter::JitAvailable();
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--interp")) run_jit = false;
        if (!std::strcmp(argv[i], "--jit")) run_interp = false;
    }
    std::printf("NeXo 2 - tests%s\n\n", Interpreter::JitAvailable() ? "" : " (compilado sin JIT)");

    int failed_tests = 0, total_tests = 0;
    for (int mode = 0; mode < 2; ++mode) {
        const bool jit = mode == 1;
        if ((jit && !run_jit) || (!jit && !run_interp)) continue;
        Interpreter::SetDefaultJitEnabled(jit);
        std::printf("===== CPU: %s =====\n", jit ? "JIT (dynarmic)" : "interprete");
        for (const auto& t : Registry()) {
            const int before = g_failures;
            t.fn();
            const bool ok = (g_failures == before);
            if (!ok) ++failed_tests;
            ++total_tests;
            std::printf("[%s] %s%s\n", ok ? " OK " : "FAIL", jit ? "[JIT] " : "", t.name);
        }
    }
    Interpreter::SetDefaultJitEnabled(false);
    std::printf("\n%d tests, %d comprobaciones, %d fallos\n", total_tests, g_checks, g_failures);
    return failed_tests == 0 ? 0 : 1;
}
