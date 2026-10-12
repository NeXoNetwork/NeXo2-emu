#include "crash_report.hpp"
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include "common/logger.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#include <unistd.h>
#endif

namespace NeXo2::Frontend {

namespace {
std::string g_logPath;
std::function<std::string()> g_context;
volatile bool g_reported = false;

FILE* OpenLog() {
    FILE* f = std::fopen(g_logPath.c_str(), "a");
    return f ? f : stderr;
}

void WriteContext(FILE* f) {
    if (!g_context) return;
    try {
        const std::string ctx = g_context();
        std::fprintf(f, "%s\n", ctx.c_str());
    } catch (...) {}
}

#ifdef _WIN32
void PrintAddress(FILE* f, HANDLE proc, DWORD64 addr) {
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
    // Modulo y desplazamiento (sirve aunque no haya .pdb)
    char module[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, module, sizeof(module));
    }
    const char* base = std::strrchr(module, '\\');
    std::fprintf(f, "    %s+0x%llX  %s+0x%llX  %s:%lu\n", base ? base + 1 : module,
                 (unsigned long long)(addr - DWORD64(mod)), has_sym ? sym->Name : "?", (unsigned long long)disp,
                 has_line ? line.FileName : "", has_line ? line.LineNumber : 0ul);
}

LONG WINAPI OnSeh(EXCEPTION_POINTERS* info) {
    if (g_reported) return EXCEPTION_CONTINUE_SEARCH;
    g_reported = true;
    FILE* f = OpenLog();
    std::fprintf(f, "\n[CRASH] NeXo se ha cerrado por un error: excepcion 0x%08lX en 0x%p\n",
                 info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        std::fprintf(f, "[CRASH] %s en la direccion 0x%llX\n", info->ExceptionRecord->ExceptionInformation[0] ? "escritura" : "lectura",
                     (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    WriteContext(f);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    std::fprintf(f, "[CRASH] Donde:\n");
    PrintAddress(f, proc, DWORD64(info->ExceptionRecord->ExceptionAddress));
    // Pila desde el punto del fallo (StackWalk64 con el contexto de la excepcion)
    std::fprintf(f, "[CRASH] Pila:\n");
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) break;
        PrintAddress(f, proc, frame.AddrPC.Offset);
    }
    std::fflush(f);
    if (f != stderr) std::fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;   // que Windows siga (cierre normal del programa)
}
#else
void OnSignal(int sig) {
    if (!g_reported) {
        g_reported = true;
        FILE* f = OpenLog();
        std::fprintf(f, "\n[CRASH] NeXo se ha cerrado por la senal %d\n", sig);
        WriteContext(f);
        void* frames[48];
        const int n = backtrace(frames, 48);
        std::fflush(f);
        backtrace_symbols_fd(frames, n, fileno(f));
        if (f != stderr) std::fclose(f);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

void OnTerminate() {
    if (!g_reported) {
        g_reported = true;
        FILE* f = OpenLog();
        std::string what = "excepcion de C++ sin capturar";
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception& e) {
            what += std::string(": ") + e.what();
        } catch (...) {}
        std::fprintf(f, "\n[CRASH] NeXo se ha cerrado: %s\n", what.c_str());
        WriteContext(f);
        if (f != stderr) std::fclose(f);
    }
    std::abort();
}
} // namespace

void InstallCrashReport(const std::string& log_path, std::function<std::string()> context) {
    g_logPath = log_path;
    g_context = std::move(context);
    std::set_terminate(OnTerminate);
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnSeh);
#else
    std::signal(SIGSEGV, OnSignal);
    std::signal(SIGBUS, OnSignal);
    std::signal(SIGFPE, OnSignal);
    std::signal(SIGILL, OnSignal);
#endif
    std::signal(SIGABRT, [](int) {
        if (!g_reported) {
            g_reported = true;
            FILE* f = OpenLog();
            std::fprintf(f, "\n[CRASH] NeXo se ha cerrado: abort()\n");
            WriteContext(f);
            if (f != stderr) std::fclose(f);
        }
    });
}

} // namespace NeXo2::Frontend
