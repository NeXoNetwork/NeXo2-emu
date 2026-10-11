#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "system.hpp"
#include "common/logger.hpp"
#include "frontend/game_list.hpp"
#include "frontend/settings.hpp"
#include "frontend/theme.hpp"
#ifdef NEXO2_HAS_VULKAN
#include "video_core/vulkan/vk_device.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <vector>
#include <string>

using NeXo2::Core::System;

// Programa ARM64 de ejemplo (se usa si no se abre ningun NRO).
// Suma 1..10 con un bucle y llama a una funcion que calcula Fibonacci(20).
// Al terminar: X0 = 55 (0x37) y X3 = 6765 (0x1A6D).
// (Para generar codigo maquina asi, ver tools/asm2cpp.py y tests/programs/.)
static const uint32_t g_program[] = {
    0xD2800000u, //      mov  x0, #0
    0xD2800141u, //      mov  x1, #10
    0x8B010000u, // 1:   add  x0, x0, x1
    0xF1000421u, //      subs x1, x1, #1
    0x54FFFFC1u, //      b.ne 1b
    0xD2800282u, //      mov  x2, #20
    0x94000002u, //      bl   fib
    0xD4200000u, //      brk  #0          <- fin: la CPU se para aqui
    0xD2800003u, // fib: mov  x3, #0
    0xD2800024u, //      mov  x4, #1
    0xB40000C2u, // 2:   cbz  x2, 3f
    0x8B040065u, //      add  x5, x3, x4
    0xAA0403E3u, //      mov  x3, x4
    0xAA0503E4u, //      mov  x4, x5
    0xD1000442u, //      sub  x2, x2, #1
    0x17FFFFFBu, //      b    2b
    0xD65F03C0u, // 3:   ret
};
static constexpr uint64_t PROG_BASE = 0x80000000ull;

static void LoadDemo(System& sys) {
    sys.LoadRawProgram(g_program, sizeof(g_program) / sizeof(g_program[0]), PROG_BASE);
}

// Ejecucion continua: en cada fotograma de la interfaz la CPU emulada corre
// durante unos milisegundos. Asi la ventana sigue respondiendo mientras tanto.
// (atomicas: las leen la interfaz y el hilo de emulacion a la vez)
static std::atomic<bool> g_emuRunning{false};
static std::atomic<unsigned long long> g_runInstructions{0};   // instrucciones desde que se pulso Run
static std::atomic<u64> g_runGeneration{0};   // sube con cada Run: el reloj real empieza de nuevo

// Medidor de velocidad: cada segundo calcula instrucciones/s e imagenes/s
struct SpeedMeter {
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    unsigned long long last_instructions = 0;
    unsigned long long last_frames = 0;
    double mips = 0.0;   // millones de instrucciones por segundo
    double fps = 0.0;    // imagenes que el programa presenta por segundo

    void Update(unsigned long long instructions, unsigned long long frames, bool running) {
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - last).count();
        if (!running) { last = now; last_instructions = instructions; last_frames = frames; mips = fps = 0.0; return; }
        if (secs < 1.0) return;
        if (instructions >= last_instructions && frames >= last_frames) {
            mips = double(instructions - last_instructions) / secs / 1e6;
            fps  = double(frames - last_frames) / secs;
        }
        last = now; last_instructions = instructions; last_frames = frames;
    }
};
static SpeedMeter g_speed;

// Candado del kernel que tiene la interfaz durante su fotograma (nullptr fuera de el)
static std::unique_lock<std::recursive_mutex>* g_uiKernelLock = nullptr;

// Modo multinucleo: parar los nucleos desde la interfaz. Hay que soltar un momento el
// candado del kernel: los nucleos lo necesitan para terminar su franja y guardar registros.
static void StopCoresFromUi(System& sys) {
    auto& k = sys.GetKernel();
    if (!k.CoresRunning()) return;
    const bool held = g_uiKernelLock && g_uiKernelLock->owns_lock();
    if (held) g_uiKernelLock->unlock();
    k.StopCores();
    if (held) g_uiKernelLock->lock();
}

// Instrucciones ejecutadas por todos los nucleos (para el medidor de velocidad)
static unsigned long long TotalInstructions(System& sys) {
    auto& k = sys.GetKernel();
    unsigned long long n = 0;
    for (int c = 0; c < NeXo2::HLE::NUM_CORES; ++c)
        if (auto* cpu = k.CoreCpu(c)) n += cpu->GetInstructionCount();
    return n;
}

static void SetRunning(System& sys, bool run) {
    if (!run) StopCoresFromUi(sys);   // antes de cargar, reiniciar... no puede quedar nada corriendo
    if (run == g_emuRunning) return;
    auto& cpu = sys.GetCpu();
    if (run && cpu.IsHalted()) return;   // parada: hay que reiniciar o cargar otro programa
    g_emuRunning = run;
    if (run) { g_runInstructions = 0; ++g_runGeneration; }
    NeXo2::Common::Logger::Log(NeXo2::Common::Logger::Level::Info,
        run ? std::string("[UI] Run") : "[UI] Pausa (" + std::to_string(g_runInstructions) + " instrucciones)");
}

// ---------------------------------------------------------------------------
//  Hilo de emulacion
// ---------------------------------------------------------------------------
// La CPU emulada corre en su propio hilo del PC, sin esperar a la interfaz.
// Un candado (mutex) protege la "consola" (System): el hilo de emulacion lo coge
// para cada tanda de ~200 000 instrucciones (~1 ms) y la interfaz lo coge una vez
// por fotograma para leer el estado y aplicar los botones. Mientras la interfaz
// espera el refresco de la pantalla (vsync), la emulacion sigue corriendo.
class EmuThread {
public:
    explicit EmuThread(System& sys) : m_sys(sys), m_thread([this] { Loop(); }) {}
    ~EmuThread() {
        m_quit = true;
        m_thread.join();
        m_sys.GetKernel().StopCores();
    }

    // La interfaz pide el candado: avisa para que el hilo de emulacion lo suelte pronto
    std::unique_lock<std::mutex> LockForUi() {
        m_uiWaiting = true;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_uiWaiting = false;
        return lock;
    }

private:
    // Modo multinucleo: cada nucleo emulado corre solo en su hilo del PC (Kernel::StartCores);
    // aqui solo se arrancan y se paran. El reloj ya es la hora real: no hace falta PaceToRealTime.
    // Devuelve false si este programa no puede ir asi (sin hilos, como la demo).
    bool RunMulticore() {
        auto& k = m_sys.GetKernel();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (k.CoresHalted()) {
                k.StopCores();
                if (m_sys.ContinueAfterExit()) {
                    ++g_runGeneration;   // el hbmenu eligio un programa (o se vuelve a el)
                } else {
                    g_emuRunning = false;
                    NeXo2::Common::Logger::Log(NeXo2::Common::Logger::Level::Info,
                        "[UI] Parada (multinucleo): " + m_sys.GetCpu().GetHaltReason());
                }
            } else if (g_emuRunning && !k.CoresRunning()) {
                k.StartCores();
                if (!k.CoresRunning()) return false;
            } else if (!g_emuRunning && k.CoresRunning()) {
                k.StopCores();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return true;
    }

    void Loop() {
        while (!m_quit) {
            if (m_sys.GetKernel().IsMulticore() && RunMulticore()) continue;
            if (!g_emuRunning || m_uiWaiting) {
                // En pausa, o la interfaz quiere el candado: ceder un momento
                std::this_thread::sleep_for(std::chrono::microseconds(g_emuRunning ? 0 : 2000));
                continue;
            }
            u64 sleep_ns = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!g_emuRunning) continue;              // la interfaz pauso mientras esperabamos
                auto& cpu = m_sys.GetCpu();
                g_runInstructions += m_sys.Run(200'000);
                sleep_ns = PaceToRealTime(cpu);
                if (cpu.IsHalted() && m_sys.ContinueAfterExit()) {
                    ++g_runGeneration;   // otro programa (lo eligio el hbmenu, o se vuelve a el)
                } else if (cpu.IsHalted()) {
                    g_emuRunning = false;
                    NeXo2::Common::Logger::Log(NeXo2::Common::Logger::Level::Info,
                        "[UI] Run: " + std::to_string(g_runInstructions.load()) + " instrucciones ejecutadas. " +
                        cpu.GetHaltReason());
                }
            }
            // Fuera del candado, para no hacer esperar a la interfaz
            if (sleep_ns) std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        }
    }

    // Reloj emulado = reloj real. El contador de la CPU (CNTPCT, svcGetSystemTick, esperas
    // de los hilos) avanza con las instrucciones. Si la emulacion va mas lenta que la consola
    // (por ejemplo esperando a la GPU por software), se adelanta el contador hasta la hora
    // real; si va mas rapida (o el programa solo espera), se duerme. Asi las animaciones
    // y los temporizadores van a la velocidad correcta.
    // Devuelve cuanto hay que dormir (ns) si la emulacion va adelantada.
    u64 PaceToRealTime(NeXo2::Core::Interpreter& cpu) {
        using Clock = std::chrono::steady_clock;
        const u64 ticks = cpu.GetTicks();
        if (!m_paceValid || ticks < m_paceLast || m_paceRun != g_runGeneration) {   // empezar de nuevo
            m_paceValid = true;
            m_paceRun = g_runGeneration;
            m_paceStart = Clock::now();
            m_paceTicks0 = ticks;
            m_paceLast = ticks;
            return 0;
        }
        constexpr double TICKS_PER_NS = double(NeXo2::Core::Interpreter::TICK_FREQUENCY) / 1e9;
        const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_paceStart).count());
        const u64 wall = m_paceTicks0 + u64(ns * TICKS_PER_NS);
        u64 sleep_ns = 0;
        if (ticks < wall) {
            cpu.AddTicks(wall - ticks);                       // la emulacion va lenta: alcanzar la hora real
        } else if (ticks > wall + u64(2e6 * TICKS_PER_NS)) { // va mas de 2 ms adelantada: esperar
            sleep_ns = u64(std::min(double(ticks - wall) / TICKS_PER_NS, 20e6));
        }
        m_paceLast = cpu.GetTicks();
        return sleep_ns;
    }

    System& m_sys;
    bool m_paceValid = false;
    u64 m_paceRun = 0, m_paceTicks0 = 0, m_paceLast = 0;
    std::chrono::steady_clock::time_point m_paceStart;
    std::mutex m_mutex;
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_uiWaiting{false};
    std::thread m_thread;   // el ultimo: arranca cuando todo lo demas ya existe
};

// ---------------------------------------------------------------------------
//  Mandos: teclado y mando del PC -> mando de la Switch
// ---------------------------------------------------------------------------
// Teclado (por posicion de la tecla, sirve para cualquier distribucion):
//   Flechas = cruceta   X = A   Z = B   S = X   A = Y
//   Q = L   W = R   1 = ZL   2 = ZR   Intro = +   Retroceso = -
//   T/F/G/H = stick izquierdo   I/J/K/L = stick derecho
// Mando (SDL, por posicion como en Nintendo): derecha = A, abajo = B, arriba = X, izquierda = Y
static SDL_Gamepad* g_gamepad = nullptr;

static NeXo2::HLE::PadInput ReadPadInput(bool use_keyboard) {
    using namespace NeXo2::HLE;
    PadInput pad;
    auto stick = [](bool neg, bool pos) { return (pos ? STICK_MAX : 0) - (neg ? STICK_MAX : 0); };

    if (use_keyboard) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        const struct { SDL_Scancode key; u64 button; } map[] = {
            {SDL_SCANCODE_UP, NpadButton::Up},     {SDL_SCANCODE_DOWN, NpadButton::Down},
            {SDL_SCANCODE_LEFT, NpadButton::Left}, {SDL_SCANCODE_RIGHT, NpadButton::Right},
            {SDL_SCANCODE_X, NpadButton::A}, {SDL_SCANCODE_Z, NpadButton::B},
            {SDL_SCANCODE_S, NpadButton::X}, {SDL_SCANCODE_A, NpadButton::Y},
            {SDL_SCANCODE_Q, NpadButton::L}, {SDL_SCANCODE_W, NpadButton::R},
            {SDL_SCANCODE_1, NpadButton::ZL}, {SDL_SCANCODE_2, NpadButton::ZR},
            {SDL_SCANCODE_RETURN, NpadButton::Plus}, {SDL_SCANCODE_BACKSPACE, NpadButton::Minus},
        };
        for (const auto& m : map) if (k[m.key]) pad.buttons |= m.button;
        pad.lx = stick(k[SDL_SCANCODE_F], k[SDL_SCANCODE_H]);
        pad.ly = stick(k[SDL_SCANCODE_G], k[SDL_SCANCODE_T]);
        pad.rx = stick(k[SDL_SCANCODE_J], k[SDL_SCANCODE_L]);
        pad.ry = stick(k[SDL_SCANCODE_K], k[SDL_SCANCODE_I]);
    }

    if (g_gamepad) {
        const struct { SDL_GamepadButton b; u64 button; } map[] = {
            {SDL_GAMEPAD_BUTTON_EAST, NpadButton::A},  {SDL_GAMEPAD_BUTTON_SOUTH, NpadButton::B},
            {SDL_GAMEPAD_BUTTON_NORTH, NpadButton::X}, {SDL_GAMEPAD_BUTTON_WEST, NpadButton::Y},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, NpadButton::L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, NpadButton::R},
            {SDL_GAMEPAD_BUTTON_START, NpadButton::Plus}, {SDL_GAMEPAD_BUTTON_BACK, NpadButton::Minus},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, NpadButton::StickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, NpadButton::StickR},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, NpadButton::Up},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, NpadButton::Down},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, NpadButton::Left}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, NpadButton::Right},
        };
        for (const auto& m : map) if (SDL_GetGamepadButton(g_gamepad, m.b)) pad.buttons |= m.button;
        if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > 16000) pad.buttons |= NpadButton::ZL;
        if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) pad.buttons |= NpadButton::ZR;
        // Ejes de SDL: -32768..32767 con Y hacia abajo. En la Switch Y va hacia arriba.
        auto axis = [](Sint16 v, bool invert) {
            int x = invert ? -int(v) : int(v);
            if (x > -6000 && x < 6000) return 0;                 // zona muerta
            return std::clamp(x, -STICK_MAX, STICK_MAX);
        };
        const s32 lx = axis(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX), false);
        const s32 ly = axis(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY), true);
        const s32 rx = axis(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX), false);
        const s32 ry = axis(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY), true);
        if (lx || ly) { pad.lx = lx; pad.ly = ly; }
        if (rx || ry) { pad.rx = rx; pad.ry = ry; }
    }
    return pad;
}

// Nombre legible del tipo de zona de memoria (MemoryState de Horizon)
static const char* StateName(NeXo2::Core::MemoryState s) {
    using NeXo2::Core::MemoryState;
    switch (s) {
        case MemoryState::Free:         return "Libre";
        case MemoryState::Static:       return "Static";
        case MemoryState::Code:         return "Code";
        case MemoryState::CodeData:     return "CodeData";
        case MemoryState::Normal:       return "Heap";
        case MemoryState::Shared:       return "Compartida";
        case MemoryState::Stack:        return "Pila";
        case MemoryState::ThreadLocal:  return "TLS";
        case MemoryState::Inaccessible: return "Inaccesible";
    }
    return "?";
}

static const char* PermName(NeXo2::Core::MemoryPermission p) {
    switch (static_cast<uint32_t>(p)) {
        case 1:  return "R--";
        case 3:  return "RW-";
        case 5:  return "R-X";
        default: return "---";
    }
}

// Apunta en el log cada boton pulsado (para depurar la interfaz)
static void LogUi(const std::string& what) {
    NeXo2::Common::Logger::Log(NeXo2::Common::Logger::Level::Info, "[UI] " + what);
}

// Ventanas de depuracion (Opciones -> Ajustes -> General -> Modo desarrollador):
// estado de la CPU, JIT, GPU, programa cargado, hilos, salida y mapa de memoria.
static void DrawDevWindows(System& sys, float main_scale, const std::string& vulkan_label, bool vulkan_ok,
                           SDL_Texture* logoTexture, char* nroPath, size_t nroPathSize, const std::string& sdmcRoot) {
    auto& cpu = sys.GetCpu();
    auto& mem = sys.GetMemory();
    auto& kernel = sys.GetKernel();
        // =====================================================================
        //  Ventana 1: CPU
        // =====================================================================
        ImGui::SetNextWindowPos(ImVec2(20 * main_scale, 70 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(760 * main_scale, 640 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("NeXo 2 Diagnostics");

        auto Tick = [](const char* label, bool ok) {
            ImGui::TextColored(ok ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1), ok ? "[OK]" : "[XX]");
            ImGui::SameLine(); ImGui::Text("%s", label);
        };

        Tick("ARM64 Interpreter", true);
        Tick("Memory (paged VMM)", mem.IsReady());
        Tick("NRO Loader + HLE Kernel (SVC basicas)", true);
        Tick("JIT: dynarmic (ARM64 -> x86-64)", NeXo2::Core::Interpreter::JitAvailable());
        Tick("SDL3 Graphics Driver", true);
        Tick("GPU Maxwell: comandos, shaders, texturas, dibujo por software (fases 1-2)", true);
        Tick(vulkan_label.c_str(), vulkan_ok);

        ImGui::Separator();
        const auto& st = cpu.GetState();
        const uint32_t curr = mem.Read<uint32_t>(st.pc);
        ImGui::Text("PC = 0x%016llX   ->   instr 0x%08X", (unsigned long long)st.pc, curr);
        ImGui::Text("SP = 0x%016llX   NZCV = %c%c%c%c   Instrucciones: %llu",
                    (unsigned long long)st.sp,
                    st.flags.n ? 'N' : '-', st.flags.z ? 'Z' : '-',
                    st.flags.c ? 'C' : '-', st.flags.v ? 'V' : '-',
                    (unsigned long long)cpu.GetInstructionCount());
        if (cpu.IsHalted())
            ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "CPU parada: %s", cpu.GetHaltReason().c_str());

        // Cache de instrucciones decodificadas: se puede apagar para comparar velocidades
        bool decodeCache = cpu.IsDecodeCacheEnabled();
        if (ImGui::Checkbox("Cache de instrucciones decodificadas", &decodeCache)) {
            LogUi(decodeCache ? "Cache de decodificacion: activada" : "Cache de decodificacion: desactivada");
            StopCoresFromUi(sys);
            cpu.SetDecodeCacheEnabled(decodeCache);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu paginas de codigo)", cpu.DecodedPages());

        // Registros X0..X30 en 3 columnas
        if (ImGui::BeginTable("regs", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            for (int i = 0; i < 31; ++i) {
                ImGui::TableNextColumn();
                ImGui::Text("X%-2d %016llX", i, (unsigned long long)st.x[i]);
            }
            ImGui::EndTable();
        }
        ImGui::Text("Paginas RAM activas: %zu (%.2f MB)",
                    mem.AllocatedPages(), mem.AllocatedBytes() / (1024.0 * 1024.0));

        if (ImGui::Button("Step CPU")) {
            LogUi("Boton Step CPU");
            if (kernel.IsMulticore()) LogUi("Step: solo en el modo de un hilo (NEXO2_MULTICORE=0)");
            else cpu.Step();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reiniciar programa")) { LogUi("Boton Reiniciar programa"); SetRunning(sys, false); sys.Restart(); }
        ImGui::SameLine();
        if (ImGui::Button(g_emuRunning ? "Pausa (F5)" : "Run (F5)")) { LogUi("Boton Run/Pausa"); SetRunning(sys, !g_emuRunning); }

        // JIT: traduce el codigo ARM64 a x86-64 (mucho mas rapido que interpretar)
        ImGui::Separator();
        if (NeXo2::Core::Interpreter::JitAvailable()) {
            bool useJit = cpu.IsJitEnabled();
            if (ImGui::Checkbox("JIT (dynarmic)", &useJit)) {
                LogUi(useJit ? "JIT: activado" : "JIT: desactivado (interprete)");
                StopCoresFromUi(sys);   // el hilo de emulacion los vuelve a arrancar con el cambio
                cpu.SetJitEnabled(useJit);
            }
            if (const auto* j = cpu.GetJit(); j && cpu.IsJitEnabled()) {
                const auto& js = j->GetStats();
                ImGui::SameLine();
                ImGui::TextDisabled("SVC %llu | interprete %llu instr | codigo reescrito %llu",
                                    (unsigned long long)js.svc_calls, (unsigned long long)js.fallbacks,
                                    (unsigned long long)js.invalidations);
            }
        } else {
            ImGui::TextDisabled("JIT no disponible (compilado sin externals/dynarmic)");
        }

        // GPU emulada: lo que ha hecho desde que se cargo el programa
        {
            auto& gpu = sys.GetKernel().GetGpu();
            auto& gs = gpu.GetStats();   // las escribe el hilo de la GPU: valores aproximados
            ImGui::Text("GPU: %s", gpu.IsAsync() ? "en su propio hilo" : "sincrona (en el hilo de la emulacion)");
            ImGui::Text("GPU: envios %llu | metodos %llu | borrados %llu | copias %llu | macros %llu",
                        (unsigned long long)gs.submits, (unsigned long long)gs.methods,
                        (unsigned long long)gs.clears, (unsigned long long)gs.copies,
                        (unsigned long long)gs.macros);
            ImGui::Text("GPU: dibujos %llu | triangulos %llu | pixeles %llu",
                        (unsigned long long)gs.draws, (unsigned long long)gs.triangles,
                        (unsigned long long)gs.pixels);
            if (gs.draws_skipped || gs.shader_errors)
                ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "GPU: %llu dibujos ignorados, %llu errores de shader (ver registro)",
                                   (unsigned long long)gs.draws_skipped, (unsigned long long)gs.shader_errors);
        }

        if (logoTexture) {
            ImGui::Separator();
            ImGui::Image((ImTextureID)(intptr_t)logoTexture, ImVec2(150, 75));
        }
        ImGui::End();

        // =====================================================================
        //  Ventana 2: Programa cargado (NRO), salida y mapa de memoria
        // =====================================================================
        ImGui::SetNextWindowPos(ImVec2(800 * main_scale, 70 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(580 * main_scale, 760 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("Programa");

        ImGui::TextWrapped("Abre un homebrew .nro (o arrastralo a la ventana):");
        ImGui::SetNextItemWidth(-110);
        ImGui::InputText("##ruta", nroPath, nroPathSize);
        ImGui::SameLine();
        if (ImGui::Button("Cargar NRO", ImVec2(-1, 0))) { LogUi("Boton Cargar NRO"); SetRunning(sys, false); sys.LoadNroFile(nroPath); }
        if (ImGui::Button("Cargar demo")) { LogUi("Boton Cargar demo"); SetRunning(sys, false); LoadDemo(sys); }
        // Controles tambien aqui, para no depender de la otra ventana
        ImGui::SameLine();
        if (ImGui::Button(g_emuRunning ? "Pausa" : "Run")) { LogUi("Boton Run/Pausa"); SetRunning(sys, !g_emuRunning); }
        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            LogUi("Boton Step");
            if (kernel.IsMulticore()) LogUi("Step: solo en el modo de un hilo (NEXO2_MULTICORE=0)");
            else cpu.Step();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reiniciar")) { LogUi("Boton Reiniciar"); SetRunning(sys, false); sys.Restart(); }
        if (cpu.IsHalted())
            ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "CPU parada: %s", cpu.GetHaltReason().c_str());
        else if (g_emuRunning)
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Ejecutando... (%llu instrucciones)",
                               (unsigned long long)g_runInstructions);
        else
            ImGui::TextDisabled("Listo para ejecutar (%llu instrucciones)",
                                (unsigned long long)cpu.GetInstructionCount());
        if (!sys.GetLastError().empty())
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", sys.GetLastError().c_str());

        ImGui::Separator();
        if (const auto* info = sys.GetNroInfo()) {
            ImGui::Text("Archivo: %s", sys.GetProgramName().c_str());
            if (!info->title.empty())
                ImGui::Text("Titulo:  %s  (%s)", info->title.c_str(), info->author.c_str());
            ImGui::Text("Cargado en 0x%llX, entrada 0x%llX, tamano 0x%llX",
                        (unsigned long long)info->base, (unsigned long long)info->entry,
                        (unsigned long long)info->image_size);
            ImGui::Text(".text 0x%X  .rodata 0x%X  .data 0x%X  .bss 0x%X",
                        info->header.text_size, info->header.ro_size,
                        info->header.data_size, info->header.bss_size);
            ImGui::Text("Handles abiertos: %zu (sesiones IPC con servicios, hilo principal...)",
                        kernel.Handles().Count());
            if (kernel.HasExited())
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "El programa ha terminado (svcExitProcess)");
        } else {
            ImGui::Text("Programa: %s", sys.GetProgramName().c_str());
        }

        ImGui::TextDisabled("Tarjeta SD (sdmc:/): %s", sdmcRoot.c_str());

        // Hilos del programa (6 nucleos emulados: a la vez en hilos del PC, o por turnos)
        if (!kernel.Threads().empty() &&
            ImGui::CollapsingHeader("Hilos", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& st = kernel.Stats();
            ImGui::Text("Nucleos: %s", kernel.IsMulticore()
                ? (kernel.CoresRunning() ? "6, cada uno en su hilo del PC (en marcha)" : "6, cada uno en su hilo del PC")
                : "6 por turnos en un hilo del PC");
            ImGui::TextDisabled("Cambios de hilo: %llu   esperas de mutex: %llu   condvar: %llu",
                                (unsigned long long)st.context_switches, (unsigned long long)st.mutex_waits,
                                (unsigned long long)st.condvar_waits);
            if (ImGui::BeginTable("hilos", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Id");
                ImGui::TableSetupColumn("Nombre");
                ImGui::TableSetupColumn("Nucleo");
                ImGui::TableSetupColumn("Prio");
                ImGui::TableSetupColumn("Estado");
                ImGui::TableSetupColumn("PC");
                ImGui::TableHeadersRow();
                for (const auto& t : kernel.Threads()) {
                    using TS = NeXo2::HLE::KThread::State;
                    using TW = NeXo2::HLE::KThread::Wait;
                    const char* state = "?";
                    switch (t->state) {
                        case TS::Created:    state = "Creado"; break;
                        case TS::Ready:      state = "Listo"; break;
                        case TS::Terminated: state = "Terminado"; break;
                        case TS::Waiting:
                            state = t->wait == TW::Sync    ? "Espera evento"
                                  : t->wait == TW::Sleep   ? "Durmiendo"
                                  : t->wait == TW::Mutex   ? "Espera mutex"
                                  : t->wait == TW::CondVar ? "Espera condvar" : "Esperando";
                            break;
                    }
                    const bool on_cpu = kernel.CurrentThread() == t.get();
                    const bool running = on_cpu || t->on_core >= 0;   // multinucleo: cargado en su nucleo
                    const unsigned long long pc = on_cpu ? cpu.GetState().pc : t->ctx.pc;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)t->id);
                    ImGui::TableNextColumn(); ImGui::Text("%s%s", t->name.c_str(), running ? " *" : "");
                    ImGui::TableNextColumn(); ImGui::Text("%d", t->core);
                    ImGui::TableNextColumn(); ImGui::Text("%d", t->priority);
                    ImGui::TableNextColumn(); ImGui::Text("%s", state);
                    ImGui::TableNextColumn(); ImGui::Text("%010llX", pc);
                }
                ImGui::EndTable();
            }
        }

        ImGui::Separator();
        ImGui::Text("Salida del programa (svcOutputDebugString):");
        ImGui::BeginChild("salida", ImVec2(0, 260), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(kernel.GetDebugOutput().c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Text("Mapa de memoria:");
        if (ImGui::BeginTable("memmap", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Inicio");
            ImGui::TableSetupColumn("Tamano");
            ImGui::TableSetupColumn("Tipo / permisos");
            ImGui::TableSetupColumn("Nombre");
            ImGui::TableHeadersRow();
            for (const auto& [base, r] : mem.Regions()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%010llX", (unsigned long long)r.base);
                ImGui::TableNextColumn(); ImGui::Text("%llX", (unsigned long long)r.size);
                ImGui::TableNextColumn(); ImGui::Text("%s %s", StateName(r.state), PermName(r.perm));
                ImGui::TableNextColumn(); ImGui::Text("%s", r.name.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::End();

}

// ===========================================================================
//  Launcher: biblioteca de juegos, barra de menus, ajustes y barra de estado
// ===========================================================================
namespace {
using namespace NeXo2::Frontend;

// Resultados de los dialogos de archivos de SDL (pueden llegar desde otro hilo)
struct DialogResults {
    std::mutex mutex;
    std::vector<std::string> open_files;   // .nro elegidos para abrir
    std::vector<std::string> add_dirs;     // carpetas de juegos nuevas
};
DialogResults g_dialogs;

void OnOpenFile(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard l(g_dialogs.mutex);
    g_dialogs.open_files.emplace_back(files[0]);
}
void OnAddFolder(void*, const char* const* files, int) {
    if (!files) return;
    std::lock_guard l(g_dialogs.mutex);
    for (int i = 0; files[i]; ++i) g_dialogs.add_dirs.emplace_back(files[i]);
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string HumanSize(u64 bytes) {
    char buf[32];
    if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", double(bytes) / double(1ull << 20));
    else std::snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(bytes >> 10));
    return buf;
}

// Texto recortado con "..." para que quepa en 'width' pixeles
std::string Ellipsize(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > width) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();   // UTF-8
    }
    return t + "...";
}

// Icono de un juego como textura (se crea la primera vez que se dibuja)
SDL_Texture* IconTexture(SDL_Renderer* r, GameEntry& g) {
    if (g.texture) return static_cast<SDL_Texture*>(g.texture);
    if (g.icon.empty()) return nullptr;
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, g.icon_w, g.icon_h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, g.icon.data(), g.icon_w * 4);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    g.texture = t;
    return t;
}
void FreeIcons(std::vector<GameEntry>& games) {
    for (auto& g : games) if (g.texture) { SDL_DestroyTexture(static_cast<SDL_Texture*>(g.texture)); g.texture = nullptr; }
}

// Icono de relleno: cuadrado oscuro con la inicial del juego
void DrawPlaceholderIcon(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::string& title, float rounding) {
    dl->AddRectFilled(a, b, IM_COL32(40, 42, 48, 255), rounding);
    const std::string letter = title.empty() ? "?" : title.substr(0, 1);
    const float size = (b.y - a.y) * 0.45f;
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, letter.c_str());
    dl->AddText(ImGui::GetFont(), size, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                IM_COL32(150, 148, 140, 255), letter.c_str());
}

void OpenFolderInExplorer(const std::filesystem::path& dir) {
    std::string url = "file:///" + PathToUtf8(dir);
    std::replace(url.begin(), url.end(), '\\', '/');
    SDL_OpenURL(url.c_str());
}
} // namespace

int main(int argc, char** argv) {
    NeXo2::Common::Logger::EnableFile("nexo2.log");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        SDL_Log("Error inicializando SDL: %s", SDL_GetError());
        return -1;
    }

    // Carpeta del ejecutable: ajustes (nexo2.ini) y tarjeta SD emulada (sdmc)
    std::string baseDir;
    if (const char* base = SDL_GetBasePath()) baseDir = base;
    const std::string iniPath = baseDir + "nexo2.ini";
    Settings settings;
    settings.Load(iniPath);
    if (settings.gpu_threads > 0 && !std::getenv("NEXO2_GPU_THREADS"))   // hilos de dibujo (al arrancar)
        SDL_setenv_unsafe("NEXO2_GPU_THREADS", std::to_string(settings.gpu_threads).c_str(), 1);

    // Escala de pantalla de Windows (100% = 1.0, 125% = 1.25...). Sin tenerla en
    // cuenta, la interfaz se ve pequena y los clics del raton caen en otro sitio.
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (main_scale <= 0.0f) main_scale = 1.0f;
    LogUi("Escala de pantalla: " + std::to_string(main_scale));
#ifndef NEXO2_VERSION
#define NEXO2_VERSION "0.0.2"
#endif
    const std::string appTitle = "NeXo 2 | " NEXO2_VERSION;
    SDL_Window* window = SDL_CreateWindow(appTitle.c_str(), (int)(1280 * main_scale), (int)(800 * main_scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1); // no dibujar mas rapido que la pantalla (ahorra CPU)

    SDL_Texture* logoTexture = nullptr;
    if (SDL_Surface* logoSurface = SDL_LoadBMP((baseDir + "assets/logo.bmp").c_str())) {
        logoTexture = SDL_CreateTextureFromSurface(renderer, logoSurface);
        SDL_DestroySurface(logoSurface);
    }

    // --- La "consola" emulada ---
    System sys;
    // Ajustes de la CPU y la GPU. Las variables de entorno (NEXO2_MULTICORE=0, NEXO2_GPU_ASYNC=0)
    // siguen mandando, para depurar.
    auto envOff = [](const char* name) { const char* e = std::getenv(name); return e && e[0] == '0'; };
    auto applyEmuSettings = [&] {
        sys.GetCpu().SetJitEnabled(settings.jit);
        sys.GetCpu().SetDecodeCacheEnabled(settings.decode_cache);
        sys.GetKernel().SetMulticore(settings.multicore && !envOff("NEXO2_MULTICORE"));
        sys.GetKernel().GetGpu().SetAsync(settings.gpu_async && !envOff("NEXO2_GPU_ASYNC"));
    };
    applyEmuSettings();

    // Sonido: audout manda PCM de 16 bits estereo a 48 kHz y aqui va a la tarjeta del PC.
    // Si se acumula mas de 1/4 de segundo (la emulacion fue a rafagas) se tira lo viejo,
    // para que el sonido no se quede cada vez mas atrasado. NEXO2_AUDIO=0 lo apaga.
    static std::atomic<int> s_volume{100};
    static std::atomic<bool> s_audioOn{true};
    s_volume = settings.volume;
    s_audioOn = settings.audio;
    SDL_AudioStream* audioStream = nullptr;
    if (!envOff("NEXO2_AUDIO") && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
        audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audioStream) {
            SDL_ResumeAudioStreamDevice(audioStream);
            sys.GetKernel().SetAudioSink([audioStream](const s16* samples, size_t frames) {
                if (!s_audioOn) return;
                constexpr int MAX_QUEUED = 48000 / 4 * 4;   // 0,25 s en bytes (2 canales x 2 bytes)
                if (SDL_GetAudioStreamQueued(audioStream) > MAX_QUEUED) SDL_ClearAudioStream(audioStream);
                const int vol = s_volume;
                if (vol >= 100) { SDL_PutAudioStreamData(audioStream, samples, int(frames * 4)); return; }
                std::vector<s16> scaled(samples, samples + frames * 2);
                for (auto& v : scaled) v = s16(int(v) * vol / 100);
                SDL_PutAudioStreamData(audioStream, scaled.data(), int(frames * 4));
            });
            LogUi("Sonido: 48000 Hz estereo");
        } else {
            LogUi(std::string("Sin sonido: ") + SDL_GetError());
        }
    }

    // Tarjeta SD emulada: carpeta "sdmc" junto al ejecutable. Su carpeta "switch" es
    // tambien la primera carpeta de juegos.
    const std::string sdmcRoot = baseDir + "sdmc";
    sys.GetKernel().SetSdmcRoot(Utf8ToPath(sdmcRoot));
    const std::filesystem::path sdSwitch = Utf8ToPath(sdmcRoot) / "switch";
    {
        std::error_code ec;
        std::filesystem::create_directories(sdSwitch, ec);
    }

    // Pantalla de la consola: textura que se actualiza con cada imagen nueva
    SDL_Texture* screenTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                                   NeXo2::HLE::Display::WIDTH, NeXo2::HLE::Display::HEIGHT);
    SDL_SetTextureBlendMode(screenTexture, SDL_BLENDMODE_NONE);   // ignorar el alfa del juego
    SDL_SetTextureScaleMode(screenTexture, settings.linear_filter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    uint64_t shownFrame = ~0ull;
    char nroPath[512] = "";

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    static std::string imguiIni;          // posiciones de las ventanas de depuracion
    imguiIni = baseDir + "imgui.ini";
    ImGui::GetIO().IniFilename = imguiIni.c_str();
    ApplyTheme(main_scale);
    const Fonts fonts = LoadFonts();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // GPU fase 3: abrir Vulkan (de momento solo para comprobar que funciona; el dibujo sigue
    // siendo por software). NEXO2_VK_VALIDATION=1 activa las capas de validacion.
#ifdef NEXO2_HAS_VULKAN
    NeXo2::GPU::Vulkan::Device vulkan;
    const bool vulkan_ok = vulkan.Init(std::getenv("NEXO2_VK_VALIDATION") != nullptr);
    const std::string vulkan_label = vulkan_ok
        ? "Vulkan Core: " + vulkan.DeviceName() + " (Vulkan " + vulkan.ApiVersionString() + ", driver " + vulkan.DriverString() + ")"
        : "Vulkan Core: " + vulkan.Error();
#else
    const bool vulkan_ok = false;
    const std::string vulkan_label = "Vulkan Core: compilado sin Vulkan";
#endif

    // ---- Biblioteca ----
    std::vector<GameEntry> games;
    std::vector<size_t> order;            // indices de 'games' en el orden de la tabla
    bool orderDirty = true;
    int selected = -1;
    char search[128] = "";
    auto gameDirs = [&] {
        std::vector<std::filesystem::path> dirs{sdSwitch};
        for (const auto& d : settings.game_dirs) dirs.push_back(Utf8ToPath(d));
        return dirs;
    };
    auto rescan = [&] {
        FreeIcons(games);
        games = ScanGames(gameDirs());
        orderDirty = true;
        selected = -1;
        LogUi("Biblioteca: " + std::to_string(games.size()) + " juegos");
    };
    rescan();

    Settings pending;                     // copia que se edita en Ajustes
    // ---- Estado del juego ----
    bool inGame = false;
    std::string gameTitle;
    std::string errorPopup;               // mensaje para la ventana de error
    bool openSettings = false, openAbout = false, openControls = false;

    auto setWindowTitle = [&] {
        SDL_SetWindowTitle(window, inGame ? (appTitle + " | " + gameTitle).c_str() : appTitle.c_str());
    };
    auto launch = [&](const std::string& path) {
        SetRunning(sys, false);
        if (audioStream) SDL_ClearAudioStream(audioStream);
        applyEmuSettings();
        std::snprintf(nroPath, sizeof(nroPath), "%s", path.c_str());
        if (!sys.LoadNroFile(path)) {
            errorPopup = "No se pudo abrir el programa:\n" + path + "\n\n" + sys.GetLastError();
            inGame = false;
            setWindowTitle();
            return;
        }
        const auto* info = sys.GetNroInfo();
        gameTitle = info && !info->title.empty() ? info->title : PathToUtf8(Utf8ToPath(path).stem());
        inGame = true;
        shownFrame = ~0ull;
        setWindowTitle();
        LogUi("Jugar: " + path);
        SetRunning(sys, true);
    };
    auto stopGame = [&] {
        SetRunning(sys, false);
        if (audioStream) SDL_ClearAudioStream(audioStream);
        inGame = false;
        setWindowTitle();
        LogUi("Detener");
    };
    auto addGameDir = [&](const std::string& dir) {
        if (dir.empty()) return;
        if (std::find(settings.game_dirs.begin(), settings.game_dirs.end(), dir) == settings.game_dirs.end()) {
            settings.game_dirs.push_back(dir);
            settings.Save(iniPath);
        }
        // Si Ajustes esta abierto, que la carpeta salga tambien en la copia que se edita
        if (std::find(pending.game_dirs.begin(), pending.game_dirs.end(), dir) == pending.game_dirs.end())
            pending.game_dirs.push_back(dir);
        rescan();
    };
    auto showOpenFile = [&] {
        static const SDL_DialogFileFilter filters[] = {{"Homebrew de Switch (*.nro)", "nro"}, {"Todos los archivos", "*"}};
        SDL_ShowOpenFileDialog(OnOpenFile, nullptr, window, filters, 2, PathToUtf8(sdSwitch).c_str(), false);
    };
    auto showAddFolder = [&] { SDL_ShowOpenFolderDialog(OnAddFolder, nullptr, window, nullptr, false); };

    // Para capturas de la interfaz: NEXO2_UI_OPEN=ajustes|acerca|controles abre esa ventana al empezar
    if (const char* e = std::getenv("NEXO2_UI_OPEN")) {
        const std::string w = e;
        openSettings = w == "ajustes";
        openAbout = w == "acerca";
        openControls = w == "controles";
    }
    // Ruta del NRO en la linea de comandos: se abre directamente
    if (argc > 1) launch(argv[1]);

    EmuThread emu(sys);   // la CPU emulada corre en su propio hilo

    bool running = true;
    while (running) {
        // Candado de la consola durante todo el fotograma de la interfaz (~1 ms);
        // se suelta antes de dibujar y esperar el vsync
        std::unique_lock<std::mutex> emuLock = emu.LockForUi();
        // Con los nucleos en sus hilos, el kernel (hilos, salida...) tiene su propio candado
        auto kernelLock = sys.GetKernel().Lock();
        g_uiKernelLock = &kernelLock;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            // Mando del PC: usamos el primero que se conecte
            if (event.type == SDL_EVENT_GAMEPAD_ADDED && !g_gamepad) {
                g_gamepad = SDL_OpenGamepad(event.gdevice.which);
                if (g_gamepad) LogUi(std::string("Mando conectado: ") + SDL_GetGamepadName(g_gamepad));
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED && g_gamepad &&
                SDL_GetGamepadID(g_gamepad) == event.gdevice.which) {
                SDL_CloseGamepad(g_gamepad);
                g_gamepad = nullptr;
                LogUi("Mando desconectado");
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
                if (event.key.key == SDLK_F5 && (inGame || g_emuRunning)) SetRunning(sys, !g_emuRunning);   // pausa
                if (event.key.key == SDLK_O && ctrl) showOpenFile();
                if (event.key.key == SDLK_COMMA && ctrl) openSettings = true;
            }
            // Arrastrar un .nro a la ventana lo abre
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) launch(event.drop.data);
        }
        // Dialogos de archivos terminados
        {
            std::vector<std::string> files, dirs;
            {
                std::lock_guard l(g_dialogs.mutex);
                files.swap(g_dialogs.open_files);
                dirs.swap(g_dialogs.add_dirs);
            }
            for (const auto& d : dirs) addGameDir(d);
            if (!files.empty()) launch(files.front());
        }

        // Mandos: el teclado solo cuenta si no se esta escribiendo en la interfaz
        if (g_emuRunning)
            sys.GetKernel().SetPadInput(ReadPadInput(!ImGui::GetIO().WantCaptureKeyboard));

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        auto& cpu = sys.GetCpu();
        auto& kernel = sys.GetKernel();

        // Imagen nueva del juego -> textura
        u64 frameCount = 0;
        {
            auto frameLock = kernel.GetDisplay().LockFrame();   // el hilo de la GPU la escribe
            const auto& frame = kernel.GetDisplay().Frame();
            frameCount = frame.count;
            if (frame.count == 0) shownFrame = ~0ull;
            if (frame.count != shownFrame && frame.count > 0 && screenTexture &&
                frame.width == NeXo2::HLE::Display::WIDTH && frame.height == NeXo2::HLE::Display::HEIGHT) {
                SDL_UpdateTexture(screenTexture, nullptr, frame.rgba.data(), int(frame.width * 4));
                shownFrame = frame.count;
            }
        }
        g_speed.Update(TotalInstructions(sys), frameCount, g_emuRunning);
        const bool showGame = inGame || g_emuRunning;

        // =====================================================================
        //  Barra de menus
        // =====================================================================
        float menuHeight = 0;
        if (ImGui::BeginMainMenuBar()) {
            menuHeight = ImGui::GetWindowSize().y;
            if (ImGui::BeginMenu("Archivo")) {
                if (ImGui::MenuItem("Abrir .nro...", "Ctrl+O")) showOpenFile();
                if (ImGui::MenuItem("Añadir carpeta de juegos...")) showAddFolder();
                if (ImGui::MenuItem("Recargar biblioteca")) rescan();
                ImGui::Separator();
                if (ImGui::MenuItem("Abrir carpeta de la SD")) OpenFolderInExplorer(Utf8ToPath(sdmcRoot));
                if (ImGui::MenuItem("Abrir carpeta de NeXo")) OpenFolderInExplorer(Utf8ToPath(baseDir));
                ImGui::Separator();
                if (ImGui::MenuItem("Salir", "Alt+F4")) running = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Emulación")) {
                const bool halted = cpu.IsHalted();
                if (ImGui::MenuItem(g_emuRunning ? "Pausa" : "Continuar", "F5", false, inGame && !halted))
                    SetRunning(sys, !g_emuRunning);
                if (ImGui::MenuItem("Detener", nullptr, false, showGame)) stopGame();
                if (ImGui::MenuItem("Reiniciar", nullptr, false, inGame)) {
                    SetRunning(sys, false);
                    if (audioStream) SDL_ClearAudioStream(audioStream);
                    sys.Restart();
                    shownFrame = ~0ull;
                    SetRunning(sys, true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Opciones")) {
                if (ImGui::MenuItem("Ajustes...", "Ctrl+,")) openSettings = true;
                if (ImGui::MenuItem("Controles...")) openControls = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Ver")) {
                if (ImGui::MenuItem("Lista", nullptr, !settings.grid_view)) { settings.grid_view = false; settings.Save(iniPath); }
                if (ImGui::MenuItem("Cuadrícula", nullptr, settings.grid_view)) { settings.grid_view = true; settings.Save(iniPath); }
                if (ImGui::BeginMenu("Tamaño de los iconos")) {
                    const char* sizes[] = {"Pequeño", "Normal", "Grande"};
                    for (int i = 0; i < 3; ++i)
                        if (ImGui::MenuItem(sizes[i], nullptr, settings.icon_size == i)) { settings.icon_size = i; settings.Save(iniPath); }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Modo desarrollador", nullptr, settings.dev_mode)) {
                    settings.dev_mode = !settings.dev_mode;
                    settings.Save(iniPath);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Ayuda")) {
                if (ImGui::MenuItem("Controles")) openControls = true;
                if (ImGui::MenuItem("Documentación")) SDL_OpenURL("https://github.com/NeXoNetwork/NeXo2-emu/tree/main/docs");
                if (ImGui::MenuItem("Página web")) SDL_OpenURL("https://nexonetwork.github.io/www/");
                if (ImGui::MenuItem("Descargar la última versión")) SDL_OpenURL("https://github.com/NeXoNetwork/NeXo2-emu/releases");
                ImGui::Separator();
                if (ImGui::MenuItem("Acerca de NeXo 2")) openAbout = true;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // =====================================================================
        //  Barra de estado (abajo)
        // =====================================================================
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float statusHeight = ImGui::GetFrameHeight() + 8.0f * main_scale;
        {
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - statusHeight));
            ImGui::SetNextWindowSize(ImVec2(vp->Size.x, statusHeight));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * main_scale, 4 * main_scale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
            ImGui::Begin("##estado", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
            ImGui::AlignTextToFramePadding();
            // Punto de color + estado
            ImVec4 dot = ImVec4(0.45f, 0.45f, 0.45f, 1);
            std::string state;
            if (cpu.IsHalted() && showGame) {
                dot = ImVec4(1.0f, 0.65f, 0.1f, 1);
                state = kernel.HasExited() ? "Terminado: " + gameTitle : "Parado: " + cpu.GetHaltReason();
            } else if (g_emuRunning) {
                dot = ImVec4(0.30f, 0.80f, 0.45f, 1);
                state = "Jugando: " + (gameTitle.empty() ? sys.GetProgramName() : gameTitle);
            } else if (inGame) {
                dot = ImVec4(0.95f, 0.75f, 0.2f, 1);
                state = "En pausa: " + gameTitle;
            } else {
                state = "Listo  ·  " + std::to_string(games.size()) + (games.size() == 1 ? " juego" : " juegos");
            }
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float r = 4.0f * main_scale;
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetFrameHeight() * 0.5f), r,
                                                            ImGui::GetColorU32(dot));
                ImGui::Dummy(ImVec2(r * 2 + 4 * main_scale, 0));
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(Ellipsize(state, vp->Size.x * 0.40f).c_str());

            // Derecha: velocidad, CPU, GPU, mando, version
            const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            const unsigned gpuThreads = settings.gpu_threads > 0 ? unsigned(settings.gpu_threads) : std::min(hw, 16u);
            char right[256];
            const std::string pad = g_gamepad ? SDL_GetGamepadName(g_gamepad) : "Teclado";
            if (g_emuRunning)
                std::snprintf(right, sizeof(right), "%.1f FPS   |   %.0f M instr/s   |   CPU: %s, %s   |   GPU: software, %u hilos   |   %s   |   v%s",
                              g_speed.fps, g_speed.mips, cpu.IsJitEnabled() ? "JIT" : "intérprete",
                              kernel.IsMulticore() ? "6 núcleos" : "1 hilo", gpuThreads, pad.c_str(), NEXO2_VERSION);
            else
                std::snprintf(right, sizeof(right), "CPU: %s, %s   |   GPU: software, %u hilos   |   %s   |   v%s",
                              settings.jit ? "JIT" : "intérprete", settings.multicore ? "6 núcleos" : "1 hilo",
                              gpuThreads, pad.c_str(), NEXO2_VERSION);
            const float rw = ImGui::CalcTextSize(right).x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20 * main_scale, ImGui::GetWindowWidth() - rw - 12 * main_scale));
            ImGui::TextDisabled("%s", right);
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        // =====================================================================
        //  Zona central: biblioteca o juego
        // =====================================================================
        const ImVec2 areaPos(vp->Pos.x, vp->Pos.y + menuHeight);
        const ImVec2 areaSize(vp->Size.x, vp->Size.y - menuHeight - statusHeight);
        ImGui::SetNextWindowPos(areaPos);
        ImGui::SetNextWindowSize(areaSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        if (showGame) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        } else {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * main_scale, 12 * main_scale));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        }
        ImGui::Begin("##central", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (showGame) {
            // ---- El juego, ocupando todo, en 16:9 ----
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            float w = avail.x, h = avail.x * 9.0f / 16.0f;
            if (h > avail.y) { h = avail.y; w = h * 16.0f / 9.0f; }
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 p0(origin.x + (avail.x - w) * 0.5f, origin.y + (avail.y - h) * 0.5f);
            if (frameCount > 0 && screenTexture && w > 1 && h > 1) {
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)screenTexture, p0, ImVec2(p0.x + w, p0.y + h));
            } else {
                const char* msg = "Cargando...";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + (avail.y - ts.y) * 0.5f),
                                                    IM_COL32(150, 148, 140, 255), msg);
            }
            // Parado (termino o fallo): aviso encima de la imagen
            if (cpu.IsHalted()) {
                const float bw = std::min(560.0f * main_scale, avail.x - 40);
                ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - bw) * 0.5f, origin.y + avail.y * 0.5f - 70 * main_scale));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.11f, 0.94f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * main_scale, 14 * main_scale));
                ImGui::BeginChild("##aviso", ImVec2(bw, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                                                           ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PushFont(fonts.bold, 0.0f);
                ImGui::TextUnformatted(kernel.HasExited() ? "El programa ha terminado" : "La emulación se ha parado");
                ImGui::PopFont();
                if (!kernel.HasExited()) ImGui::TextWrapped("%s", cpu.GetHaltReason().c_str());
                ImGui::Spacing();
                if (ImGui::Button("Volver a la biblioteca")) stopGame();
                ImGui::SameLine();
                if (ImGui::Button("Reiniciar")) {
                    if (audioStream) SDL_ClearAudioStream(audioStream);
                    sys.Restart();
                    shownFrame = ~0ull;
                    inGame = true;
                    SetRunning(sys, true);
                }
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
        } else {
            // ---- Biblioteca ----
            // Barra de herramientas: buscar, vista, carpetas
            ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.35f);
            ImGui::TextUnformatted("Biblioteca");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%zu %s", games.size(), games.size() == 1 ? "juego" : "juegos");

            const ImGuiStyle& stl = ImGui::GetStyle();
            auto btnW = [&](const char* t) { return ImGui::CalcTextSize(t).x + stl.FramePadding.x * 2; };
            const float toolsW = 220 * main_scale + btnW("Lista") + btnW("Cuadrícula") + btnW("+ Carpeta") + btnW("Recargar") +
                                 stl.ItemSpacing.x * 3 + 4 * main_scale + 2;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowContentRegionMax().x - toolsW));
            ImGui::SetNextItemWidth(220 * main_scale);
            if (ImGui::InputTextWithHint("##buscar", "Buscar...", search, sizeof(search))) orderDirty = true;
            ImGui::SameLine();
            auto toggle = [&](const char* label, bool on) {
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                const bool clicked = ImGui::Button(label);
                if (on) ImGui::PopStyleColor();
                return clicked;
            };
            if (toggle("Lista", !settings.grid_view)) { settings.grid_view = false; settings.Save(iniPath); }
            ImGui::SameLine(0, 4 * main_scale);
            if (toggle("Cuadrícula", settings.grid_view)) { settings.grid_view = true; settings.Save(iniPath); }
            ImGui::SameLine();
            if (ImGui::Button("+ Carpeta")) showAddFolder();
            ImGui::SameLine();
            if (ImGui::Button("Recargar")) rescan();
            ImGui::Spacing();

            // Filtro y orden
            const std::string needle = ToLower(search);
            if (orderDirty) {
                order.clear();
                for (size_t i = 0; i < games.size(); ++i) {
                    if (!needle.empty() && ToLower(games[i].title).find(needle) == std::string::npos &&
                        ToLower(games[i].author).find(needle) == std::string::npos &&
                        ToLower(games[i].file_name).find(needle) == std::string::npos)
                        continue;
                    order.push_back(i);
                }
                orderDirty = false;
            }

            const float iconPx[] = {32.0f, 48.0f, 64.0f};
            const float cardPx[] = {120.0f, 160.0f, 210.0f};
            if (games.empty()) {
                // Sin juegos: explicar donde ponerlos
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float bw = std::min(620.0f * main_scale, avail.x);
                ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - bw) * 0.5f, ImGui::GetCursorPosY() + avail.y * 0.25f));
                ImGui::BeginChild("##vacio", ImVec2(bw, 0), ImGuiChildFlags_AutoResizeY);
                ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.25f);
                ImGui::TextUnformatted("Todavía no hay juegos");
                ImGui::PopFont();
                ImGui::TextWrapped("Copia tus homebrew (.nro) en la carpeta de la SD emulada o añade una carpeta "
                                   "donde los tengas. NeXo los busca también en las subcarpetas.");
                ImGui::TextDisabled("%s", PathToUtf8(sdSwitch).c_str());
                ImGui::Spacing();
                if (ImGui::Button("Abrir carpeta de la SD")) OpenFolderInExplorer(sdSwitch);
                ImGui::SameLine();
                if (ImGui::Button("Añadir carpeta...")) showAddFolder();
                ImGui::SameLine();
                if (ImGui::Button("Abrir un .nro...")) showOpenFile();
                ImGui::EndChild();
            } else if (!settings.grid_view) {
                // ---- Vista de lista (tabla ordenable) ----
                const float ip = iconPx[settings.icon_size] * main_scale;
                const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
                if (ImGui::BeginTable("##juegos", 6, flags)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("##icono", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort |
                                                       ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoHide, ip);
                    ImGui::TableSetupColumn("Nombre", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, 3.0f);
                    ImGui::TableSetupColumn("Autor", 0, 2.0f);
                    ImGui::TableSetupColumn("Versión", 0, 0.8f);
                    ImGui::TableSetupColumn("Tamaño", ImGuiTableColumnFlags_PreferSortDescending, 0.8f);
                    ImGui::TableSetupColumn("Archivo", 0, 2.0f);
                    ImGui::TableHeadersRow();
                    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && (specs->SpecsDirty || orderDirty)) {
                        if (specs->SpecsCount > 0) {
                            const auto& s = specs->Specs[0];
                            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                                const GameEntry& x = games[a];
                                const GameEntry& y = games[b];
                                int c = 0;
                                switch (s.ColumnIndex) {
                                    case 2: c = ToLower(x.author).compare(ToLower(y.author)); break;
                                    case 3: c = x.version.compare(y.version); break;
                                    case 4: c = x.size < y.size ? -1 : x.size > y.size ? 1 : 0; break;
                                    case 5: c = ToLower(x.file_name).compare(ToLower(y.file_name)); break;
                                    default: c = ToLower(x.title).compare(ToLower(y.title)); break;
                                }
                                return s.SortDirection == ImGuiSortDirection_Descending ? c > 0 : c < 0;
                            });
                        }
                        specs->SpecsDirty = false;
                    }
                    for (size_t idx : order) {
                        GameEntry& g = games[idx];
                        ImGui::PushID(int(idx));
                        ImGui::TableNextRow(0, ip + 6 * main_scale);
                        ImGui::TableNextColumn();
                        // Toda la fila se puede elegir: doble clic = jugar, clic derecho = menu
                        const bool isSel = selected == int(idx);
                        if (ImGui::Selectable("##fila", isSel, ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                                              ImVec2(0, ip + 2 * main_scale))) {
                            selected = int(idx);
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch(g.path);
                        }
                        if (ImGui::BeginPopupContextItem("##menu")) {
                            selected = int(idx);
                            if (ImGui::MenuItem("Jugar")) launch(g.path);
                            if (ImGui::MenuItem("Abrir la carpeta")) OpenFolderInExplorer(Utf8ToPath(g.path).parent_path());
                            if (ImGui::MenuItem("Copiar la ruta")) SDL_SetClipboardText(g.path.c_str());
                            ImGui::EndPopup();
                        }
                        // Icono en la primera columna (encima de la fila elegible)
                        const ImVec2 cell = ImGui::GetItemRectMin();
                        const ImVec2 i0(cell.x, cell.y + 1 * main_scale), i1(cell.x + ip, cell.y + 1 * main_scale + ip);
                        if (SDL_Texture* t = IconTexture(renderer, g))
                            ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)(intptr_t)t, i0, i1, ImVec2(0, 0), ImVec2(1, 1),
                                                                        IM_COL32_WHITE, 4 * main_scale);
                        else
                            DrawPlaceholderIcon(ImGui::GetWindowDrawList(), i0, i1, g.title, 4 * main_scale);
                        // Texto centrado en vertical
                        const float textY = (ip + 2 * main_scale - ImGui::GetTextLineHeight()) * 0.5f;
                        auto cellText = [&](const std::string& s, bool dim) {
                            ImGui::TableNextColumn();
                            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textY);
                            if (dim) ImGui::TextDisabled("%s", s.c_str()); else ImGui::TextUnformatted(s.c_str());
                        };
                        cellText(g.title, false);
                        cellText(g.author, true);
                        cellText(g.version, true);
                        cellText(HumanSize(g.size), true);
                        cellText(g.file_name, true);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            } else {
                // ---- Vista de cuadricula ----
                ImGui::BeginChild("##rejilla", ImVec2(0, 0));
                const float card = cardPx[settings.icon_size] * main_scale;
                const float gap = 14 * main_scale;
                const float textH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 6 * main_scale;
                const float availW = ImGui::GetContentRegionAvail().x;
                const int cols = std::max(1, int((availW + gap) / (card + gap)));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (size_t n = 0; n < order.size(); ++n) {
                    GameEntry& g = games[order[n]];
                    if (n % size_t(cols) != 0) ImGui::SameLine(0, gap);
                    ImGui::PushID(int(order[n]));
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##carta", ImVec2(card, card + textH));
                    const bool hovered = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked()) selected = int(order[n]);
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch(g.path);
                    if (ImGui::BeginPopupContextItem("##menu")) {
                        selected = int(order[n]);
                        if (ImGui::MenuItem("Jugar")) launch(g.path);
                        if (ImGui::MenuItem("Abrir la carpeta")) OpenFolderInExplorer(Utf8ToPath(g.path).parent_path());
                        if (ImGui::MenuItem("Copiar la ruta")) SDL_SetClipboardText(g.path.c_str());
                        ImGui::EndPopup();
                    }
                    const bool isSel = selected == int(order[n]);
                    const float rr = 8 * main_scale;
                    if (hovered || isSel)
                        dl->AddRectFilled(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + card + 4, p.y + card + textH + 2),
                                          IM_COL32(255, 255, 255, hovered ? 14 : 8), rr + 2);
                    const ImVec2 i0 = p, i1(p.x + card, p.y + card);
                    if (SDL_Texture* t = IconTexture(renderer, g))
                        dl->AddImageRounded((ImTextureID)(intptr_t)t, i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rr);
                    else
                        DrawPlaceholderIcon(dl, i0, i1, g.title, rr);
                    if (isSel) dl->AddRect(ImVec2(i0.x - 2, i0.y - 2), ImVec2(i1.x + 2, i1.y + 2), ACCENT, rr + 2, 0, 2.5f * main_scale);
                    const float ty = p.y + card + 6 * main_scale;
                    dl->AddText(ImVec2(p.x, ty), ImGui::GetColorU32(ImGuiCol_Text), Ellipsize(g.title, card).c_str());
                    dl->AddText(ImVec2(p.x, ty + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                Ellipsize(g.author.empty() ? g.version : g.author, card).c_str());
                    if (hovered) ImGui::SetTooltip("%s\n%s%s%s\nDoble clic para jugar", g.title.c_str(), g.author.c_str(),
                                                   g.version.empty() ? "" : "  ·  v", g.version.c_str());
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
            // Intro = jugar el seleccionado
            if (selected >= 0 && selected < int(games.size()) && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                launch(games[size_t(selected)].path);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        // =====================================================================
        //  Ventanas de depuracion (modo desarrollador)
        // =====================================================================
        if (settings.dev_mode)
            DrawDevWindows(sys, main_scale, vulkan_label, vulkan_ok, logoTexture, nroPath, sizeof(nroPath), sdmcRoot);

        // =====================================================================
        //  Ajustes
        // =====================================================================
        if (openSettings) { pending = settings; ImGui::OpenPopup("Ajustes"); openSettings = false; }
        ImGui::SetNextWindowSize(ImVec2(640 * main_scale, 480 * main_scale), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Ajustes", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            const float footer = ImGui::GetFrameHeightWithSpacing() + 10 * main_scale;
            ImGui::BeginChild("##pestanas", ImVec2(0, -footer));
            auto note = [](const char* text) {
                ImGui::Indent();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", text);
                ImGui::PopStyleColor();
                ImGui::Unindent();
            };
            if (ImGui::BeginTabBar("##ajustes")) {
                if (ImGui::BeginTabItem("General")) {
                    ImGui::SeparatorText("Carpetas de juegos");
                    ImGui::TextDisabled("%s  (SD emulada, siempre)", PathToUtf8(sdSwitch).c_str());
                    int remove = -1;
                    for (size_t i = 0; i < pending.game_dirs.size(); ++i) {
                        ImGui::PushID(int(i));
                        if (ImGui::SmallButton("Quitar")) remove = int(i);
                        ImGui::SameLine();
                        ImGui::TextUnformatted(pending.game_dirs[i].c_str());
                        ImGui::PopID();
                    }
                    if (remove >= 0) pending.game_dirs.erase(pending.game_dirs.begin() + remove);
                    if (ImGui::Button("Añadir carpeta...")) showAddFolder();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(se guarda al momento)");
                    ImGui::SeparatorText("Interfaz");
                    ImGui::Checkbox("Modo desarrollador", &pending.dev_mode);
                    note("Muestra las ventanas de depuración: CPU, JIT, GPU, hilos, salida del programa y mapa de memoria.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("CPU")) {
                    ImGui::Checkbox("JIT (dynarmic)", &pending.jit);
                    note("Traduce el código ARM64 a x86-64: unas 6 veces más rápido que el intérprete.");
                    ImGui::Checkbox("6 núcleos en hilos del PC", &pending.multicore);
                    note("Cada núcleo emulado en su propio hilo. Si lo quitas, se turnan en uno (más lento, pero determinista).");
                    ImGui::Checkbox("Caché de instrucciones decodificadas", &pending.decode_cache);
                    note("Solo para el intérprete.");
                    ImGui::Spacing();
                    ImGui::TextDisabled("Los cambios se aplican al abrir el siguiente juego.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Gráficos")) {
                    ImGui::TextUnformatted("Motor: GPU por software (Vulkan en desarrollo)");
                    ImGui::TextDisabled("%s", vulkan_label.c_str());
                    ImGui::Spacing();
                    ImGui::Checkbox("GPU en su propio hilo", &pending.gpu_async);
                    note("Dibuja mientras la CPU emulada sigue. Se aplica al abrir el siguiente juego.");
                    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
                    ImGui::SetNextItemWidth(200 * main_scale);
                    ImGui::SliderInt("Hilos de dibujo", &pending.gpu_threads, 0, int(std::min(hw, 32u)),
                                     pending.gpu_threads == 0 ? "Automático" : "%d");
                    note("Automático = todos los del PC (máximo 16). Se aplica al reiniciar NeXo.");
                    ImGui::Checkbox("Suavizar la imagen al escalar", &pending.linear_filter);
                    note("Sin suavizado se ven los píxeles nítidos.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio")) {
                    ImGui::Checkbox("Sonido", &pending.audio);
                    ImGui::SetNextItemWidth(260 * main_scale);
                    ImGui::SliderInt("Volumen", &pending.volume, 0, 100, "%d %%");
                    if (!audioStream) ImGui::TextColored(ImVec4(1, 0.65f, 0.1f, 1), "No se pudo abrir el dispositivo de sonido.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controles")) {
                    ImGui::Text("Mando: %s", g_gamepad ? SDL_GetGamepadName(g_gamepad) : "ninguno conectado");
                    note("Se usa el primer mando que se conecte, con los botones en la posición de Nintendo.");
                    ImGui::SeparatorText("Teclado");
                    if (ImGui::BeginTable("##teclas", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                        const char* rows[][2] = {
                            {"Cruceta", "Flechas"}, {"A / B / X / Y", "X / Z / S / A"}, {"L / R", "Q / W"},
                            {"ZL / ZR", "1 / 2"}, {"+ / -", "Intro / Retroceso"},
                            {"Stick izquierdo", "T / F / G / H"}, {"Stick derecho", "I / J / K / L"},
                        };
                        for (const auto& r : rows) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(r[0]);
                            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r[1]);
                        }
                        ImGui::EndTable();
                    }
                    note("Las teclas van por posición: sirven en cualquier distribución de teclado.");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::Separator();
            const float bw = 110 * main_scale;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Button("Cancelar", ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
            if (ImGui::Button("Guardar", ImVec2(bw, 0))) {
                // Las carpetas se guardan al momento (dialogo): conservar las actuales menos las quitadas
                pending.game_dirs.erase(std::remove_if(pending.game_dirs.begin(), pending.game_dirs.end(),
                                                       [](const std::string& d) { return d.empty(); }),
                                        pending.game_dirs.end());
                const bool dirsChanged = pending.game_dirs != settings.game_dirs;
                const bool cpuChanged = pending.jit != settings.jit || pending.decode_cache != settings.decode_cache;
                settings = pending;
                settings.Save(iniPath);
                s_audioOn = settings.audio;
                s_volume = settings.volume;
                SDL_SetTextureScaleMode(screenTexture, settings.linear_filter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
                if (!showGame && cpuChanged) {
                    StopCoresFromUi(sys);
                    sys.GetCpu().SetJitEnabled(settings.jit);
                    sys.GetCpu().SetDecodeCacheEnabled(settings.decode_cache);
                }
                if (dirsChanged) rescan();
                LogUi("Ajustes guardados");
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }

        // =====================================================================
        //  Controles, Acerca de, errores
        // =====================================================================
        if (openControls) { ImGui::OpenPopup("Controles"); openControls = false; }
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Controles", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Text("Mando: %s", g_gamepad ? SDL_GetGamepadName(g_gamepad) : "ninguno conectado");
            ImGui::SeparatorText("Teclado");
            ImGui::TextUnformatted("Flechas = cruceta    X = A    Z = B    S = X    A = Y\n"
                                   "Q = L    W = R    1 = ZL    2 = ZR    Intro = +    Retroceso = -\n"
                                   "T/F/G/H = stick izquierdo    I/J/K/L = stick derecho");
            ImGui::SeparatorText("NeXo");
            ImGui::TextUnformatted("F5 = pausa / continuar    Ctrl+O = abrir .nro    Ctrl+, = ajustes");
            ImGui::Spacing();
            if (ImGui::Button("Cerrar", ImVec2(120 * main_scale, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (openAbout) { ImGui::OpenPopup("Acerca de NeXo 2"); openAbout = false; }
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Acerca de NeXo 2", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            if (logoTexture) ImGui::Image((ImTextureID)(intptr_t)logoTexture, ImVec2(150 * main_scale, 75 * main_scale));
            ImGui::PushFont(fonts.bold, ImGui::GetStyle().FontSizeBase * 1.3f);
            ImGui::Text("NeXo 2  v%s", NEXO2_VERSION);
            ImGui::PopFont();
            ImGui::TextUnformatted("Emulador experimental de Nintendo Switch 2, escrito desde cero en C++20.");
            ImGui::TextDisabled("Software libre (GPLv3). No incluye firmware, claves ni juegos.");
            ImGui::TextDisabled("Sin relación con Nintendo.");
            ImGui::Spacing();
            if (ImGui::Button("GitHub")) SDL_OpenURL("https://github.com/NeXoNetwork/NeXo2-emu");
            ImGui::SameLine();
            if (ImGui::Button("Cerrar", ImVec2(120 * main_scale, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!errorPopup.empty()) ImGui::OpenPopup("Error");
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushTextWrapPos(520 * main_scale);
            ImGui::TextUnformatted(errorPopup.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Aceptar", ImVec2(120 * main_scale, 0))) { errorPopup.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::Render();
        g_uiKernelLock = nullptr;
        kernelLock.unlock();
        emuLock.unlock();   // a partir de aqui no se toca la consola: la emulacion sigue

        SDL_SetRenderDrawColor(renderer, 17, 18, 21, 255);
        SDL_RenderClear(renderer);
        // Dibujar en pixeles reales (pantallas con escala != 100%)
        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    FreeIcons(games);
    if (logoTexture) SDL_DestroyTexture(logoTexture);
    if (screenTexture) SDL_DestroyTexture(screenTexture);
    if (g_gamepad) SDL_CloseGamepad(g_gamepad);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    sys.GetKernel().SetAudioSink(nullptr);
    if (audioStream) SDL_DestroyAudioStream(audioStream);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
