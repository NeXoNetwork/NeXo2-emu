#pragma once
// Mini framework de tests de NeXo 2 (sin dependencias externas).
//
//   TEST(Nombre) { CHECK(condicion); CHECK_EQ(valor, esperado); }
//
// Cada TEST se registra solo; tests/test_main.cpp los ejecuta todos.
#include <cstdio>
#include <functional>
#include <vector>

namespace NeXo2::Tests {
inline int g_failures = 0;
inline int g_checks   = 0;

struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); } };
} // namespace NeXo2::Tests

#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        ++NeXo2::Tests::g_checks;                                                        \
        const unsigned long long a_ = (unsigned long long)(actual);                      \
        const unsigned long long e_ = (unsigned long long)(expected);                    \
        if (a_ != e_) {                                                                  \
            ++NeXo2::Tests::g_failures;                                                  \
            std::printf("    FALLO %s:%d  %s = 0x%llX, esperado 0x%llX\n",               \
                        __FILE__, __LINE__, #actual, a_, e_);                            \
        }                                                                                \
    } while (0)

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++NeXo2::Tests::g_checks;                                                        \
        if (!(cond)) {                                                                   \
            ++NeXo2::Tests::g_failures;                                                  \
            std::printf("    FALLO %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                                \
    } while (0)

#define TEST(name)                                                                       \
    static void name();                                                                  \
    static NeXo2::Tests::Registrar reg_##name(#name, name);                              \
    static void name()
