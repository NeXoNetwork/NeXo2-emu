// Punto de entrada de nexo2_tests: ejecuta todos los TEST(...) registrados
// en cpu_tests.cpp, loader_tests.cpp, etc.
//
// Si NeXo se compilo con el JIT, todos los tests se ejecutan DOS veces: con el
// interprete y con el JIT (todas las CPUs que creen los tests usan el JIT).
//   nexo2_tests            -> los dos
//   nexo2_tests --interp   -> solo interprete
//   nexo2_tests --jit      -> solo JIT
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "test_framework.hpp"
#include "arm64/interpreter.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

// Si un test revienta (acceso a memoria invalido, abort...), decir cual era antes de morir.
// Sin esto, en GitHub Actions (salida por tuberia, con buffer) se pierden las ultimas lineas
// y no se sabe donde fallo.
static const char* volatile g_currentTest = "(ninguno)";
static volatile bool g_currentJit = false;

static void ReportCrash(const char* what) {
    std::fprintf(stdout, "\n[CRASH] %s en el test: %s%s\n", what, g_currentJit ? "[JIT] " : "", g_currentTest);
    std::fflush(stdout);
}
static void OnSignal(int sig) {
    ReportCrash(sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : sig == SIGFPE ? "SIGFPE" : "senal");
    std::_Exit(3);
}
#ifdef _WIN32
// Nombre de funcion y linea de una direccion (necesita el .pdb: en Release se genera igual)
static void PrintAddress(HANDLE proc, DWORD64 addr) {
    alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    const bool has_sym = SymFromAddr(proc, addr, &disp, sym) != FALSE;
    const bool has_line = SymGetLineFromAddr64(proc, addr, &ldisp, &line) != FALSE;
    std::fprintf(stdout, "    0x%016llX  %s+0x%llX  %s:%lu\n", (unsigned long long)addr,
                 has_sym ? sym->Name : "?", (unsigned long long)disp,
                 has_line ? line.FileName : "", has_line ? line.LineNumber : 0ul);
}

static LONG WINAPI OnSehException(EXCEPTION_POINTERS* info) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Excepcion 0x%08lX en 0x%p", info->ExceptionRecord->ExceptionCode,
                  info->ExceptionRecord->ExceptionAddress);
    ReportCrash(buf);
    // Pila de llamadas: donde estaba cada funcion cuando revento
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(proc, nullptr, TRUE)) {
        std::fprintf(stdout, "  Donde revento:\n");
        PrintAddress(proc, DWORD64(info->ExceptionRecord->ExceptionAddress));
        std::fprintf(stdout, "  Pila:\n");
        void* frames[48];
        const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) PrintAddress(proc, DWORD64(frames[i]));
    }
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
    using namespace NeXo2::Tests;
    using NeXo2::Core::Interpreter;
    bool run_interp = true, run_jit = Interpreter::JitAvailable();
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--interp")) run_jit = false;
        if (!std::strcmp(argv[i], "--jit")) run_interp = false;
    }
    // Salida sin buffer: lo escrito antes de un fallo no se pierde
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);
#ifndef _WIN32
    std::signal(SIGSEGV, OnSignal);   // en Windows lo hace OnSehException (con la pila)
    std::signal(SIGFPE, OnSignal);
#endif
    std::signal(SIGABRT, OnSignal);
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnSehException);
    // Sin ventanas de "el programa dejo de funcionar" (en CI se quedaria colgado)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    std::printf("NeXo 2 - tests%s\n\n", Interpreter::JitAvailable() ? "" : " (compilado sin JIT)");

    int failed_tests = 0, total_tests = 0;
    for (int mode = 0; mode < 2; ++mode) {
        const bool jit = mode == 1;
        if ((jit && !run_jit) || (!jit && !run_interp)) continue;
        Interpreter::SetDefaultJitEnabled(jit);
        std::printf("===== CPU: %s =====\n", jit ? "JIT (dynarmic)" : "interprete");
        const char* filter = std::getenv("NEXO2_TEST_FILTER");   // solo los tests cuyo nombre contenga esto
        for (const auto& t : Registry()) {
            if (filter && !std::strstr(t.name, filter)) continue;
            const int before = g_failures;
            g_currentTest = t.name;
            g_currentJit = jit;
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
