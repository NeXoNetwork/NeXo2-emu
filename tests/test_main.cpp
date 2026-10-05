// Punto de entrada de nexo2_tests: ejecuta todos los TEST(...) registrados
// en cpu_tests.cpp, loader_tests.cpp, etc.
#include <cstdio>
#include "test_framework.hpp"

int main() {
    using namespace NeXo2::Tests;
    std::printf("NeXo 2 - tests\n\n");
    int failed_tests = 0;
    for (const auto& t : Registry()) {
        const int before = g_failures;
        t.fn();
        const bool ok = (g_failures == before);
        if (!ok) ++failed_tests;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%zu tests, %d comprobaciones, %d fallos\n",
                Registry().size(), g_checks, g_failures);
    return failed_tests == 0 ? 0 : 1;
}
