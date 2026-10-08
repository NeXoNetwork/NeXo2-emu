#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "system.hpp"
#include "common/logger.hpp"
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
#include <cstring>
#include <filesystem>
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

static void SetRunning(System& sys, bool run) {
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
    }

    // La interfaz pide el candado: avisa para que el hilo de emulacion lo suelte pronto
    std::unique_lock<std::mutex> LockForUi() {
        m_uiWaiting = true;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_uiWaiting = false;
        return lock;
    }

private:
    void Loop() {
        while (!m_quit) {
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
                if (cpu.IsHalted()) {
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

int main(int argc, char** argv) {
    NeXo2::Common::Logger::EnableFile("nexo2.log");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        SDL_Log("Error inicializando SDL: %s", SDL_GetError());
        return -1;
    }

    // Escala de pantalla de Windows (100% = 1.0, 125% = 1.25...). Sin tenerla en
    // cuenta, la interfaz se ve pequena y los clics del raton caen en otro sitio.
    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (main_scale <= 0.0f) main_scale = 1.0f;
    LogUi("Escala de pantalla: " + std::to_string(main_scale));
    SDL_Window* window = SDL_CreateWindow("NeXo 2 | 0.0.0.2", (int)(1400 * main_scale), (int)(800 * main_scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1); // no dibujar mas rapido que la pantalla (ahorra CPU)

    SDL_Texture* logoTexture = nullptr;
    if (SDL_Surface* logoSurface = SDL_LoadBMP("assets/logo.bmp")) {
        logoTexture = SDL_CreateTextureFromSurface(renderer, logoSurface);
        SDL_DestroySurface(logoSurface);
    }

    // --- La "consola" emulada ---
    System sys;
    // JIT activado por defecto (si NeXo se compilo con dynarmic). Se puede apagar en Diagnostics.
    sys.GetCpu().SetJitEnabled(true);

    // Tarjeta SD emulada: carpeta "sdmc" junto al ejecutable
    std::string sdmcRoot = "sdmc";
    if (const char* base = SDL_GetBasePath()) sdmcRoot = std::string(base) + "sdmc";
    sys.GetKernel().SetSdmcRoot(std::filesystem::path(std::u8string(sdmcRoot.begin(), sdmcRoot.end())));

    // Pantalla de la consola: textura que se actualiza con cada imagen nueva
    SDL_Texture* screenTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                                   NeXo2::HLE::Display::WIDTH, NeXo2::HLE::Display::HEIGHT);
    SDL_SetTextureBlendMode(screenTexture, SDL_BLENDMODE_NONE);   // ignorar el alfa del juego
    SDL_SetTextureScaleMode(screenTexture, SDL_SCALEMODE_LINEAR);
    uint64_t shownFrame = ~0ull;

    // Ruta del NRO: la de la linea de comandos o el homebrew de prueba
    char nroPath[512] = "tests/generated/hello.nro";
    if (argc > 1) {
        std::snprintf(nroPath, sizeof(nroPath), "%s", argv[1]);
        if (!sys.LoadNroFile(nroPath)) LoadDemo(sys);
    } else {
        LoadDemo(sys);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);   // botones, margenes... a la escala de la pantalla
    style.FontScaleDpi = main_scale;   // texto a la escala de la pantalla
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

    EmuThread emu(sys);   // la CPU emulada corre en su propio hilo

    bool running = true;
    while (running) {
        // Candado de la consola durante todo el fotograma de la interfaz (~1 ms);
        // se suelta antes de dibujar y esperar el vsync
        std::unique_lock<std::mutex> emuLock = emu.LockForUi();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                LogUi("Clic raton en (" + std::to_string((int)event.button.x) + ", " +
                      std::to_string((int)event.button.y) + ")");
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5) LogUi("Tecla F5");
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
            // F5 = Run / Pausa
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5 && !event.key.repeat)
                SetRunning(sys, !g_emuRunning);
            // Arrastrar un .nro a la ventana lo carga
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                std::snprintf(nroPath, sizeof(nroPath), "%s", event.drop.data);
                SetRunning(sys, false);
                sys.LoadNroFile(nroPath);
            }
        }

        // Mandos: el teclado solo cuenta si no se esta escribiendo en la interfaz
        if (g_emuRunning)
            sys.GetKernel().SetPadInput(ReadPadInput(!ImGui::GetIO().WantCaptureKeyboard));

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        auto& cpu = sys.GetCpu();
        auto& mem = sys.GetMemory();
        auto& kernel = sys.GetKernel();

        // =====================================================================
        //  Ventana 1: CPU
        // =====================================================================
        ImGui::SetNextWindowPos(ImVec2(20 * main_scale, 20 * main_scale), ImGuiCond_FirstUseEver);
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

        if (ImGui::Button("Step CPU")) { LogUi("Boton Step CPU"); cpu.Step(); }
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
            auto& gs = sys.GetKernel().GetGpu().GetStats();
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
        ImGui::SetNextWindowPos(ImVec2(800 * main_scale, 20 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(580 * main_scale, 760 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("Programa");

        ImGui::TextWrapped("Abre un homebrew .nro (o arrastralo a la ventana):");
        ImGui::SetNextItemWidth(-110);
        ImGui::InputText("##ruta", nroPath, sizeof(nroPath));
        ImGui::SameLine();
        if (ImGui::Button("Cargar NRO", ImVec2(-1, 0))) { LogUi("Boton Cargar NRO"); SetRunning(sys, false); sys.LoadNroFile(nroPath); }
        if (ImGui::Button("Cargar demo")) { LogUi("Boton Cargar demo"); SetRunning(sys, false); LoadDemo(sys); }
        // Controles tambien aqui, para no depender de la otra ventana
        ImGui::SameLine();
        if (ImGui::Button(g_emuRunning ? "Pausa" : "Run")) { LogUi("Boton Run/Pausa"); SetRunning(sys, !g_emuRunning); }
        ImGui::SameLine();
        if (ImGui::Button("Step")) { LogUi("Boton Step"); cpu.Step(); }
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

        // Hilos del programa (6 nucleos emulados que se turnan)
        if (!kernel.Threads().empty() &&
            ImGui::CollapsingHeader("Hilos", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& st = kernel.Stats();
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
                    const bool running = kernel.CurrentThread() == t.get();
                    const unsigned long long pc = running ? cpu.GetState().pc : t->ctx.pc;
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

        // =====================================================================
        //  Ventana 3: Pantalla de la consola (lo que el programa manda a vi)
        // =====================================================================
        const auto& frame = kernel.GetDisplay().Frame();
        g_speed.Update(cpu.GetInstructionCount(), frame.count, g_emuRunning);
        if (frame.count == 0) shownFrame = ~0ull;   // programa nuevo: la siguiente imagen se muestra seguro
        if (frame.count != shownFrame && frame.count > 0 && screenTexture &&
            frame.width == NeXo2::HLE::Display::WIDTH && frame.height == NeXo2::HLE::Display::HEIGHT) {
            SDL_UpdateTexture(screenTexture, nullptr, frame.rgba.data(), int(frame.width * 4));
            shownFrame = frame.count;
        }
        ImGui::SetNextWindowPos(ImVec2(60 * main_scale, 80 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(660 * main_scale, 420 * main_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("Pantalla");
        if (frame.count > 0 && screenTexture) {
            ImGui::Text("Imagenes: %llu   Mando: %s", (unsigned long long)frame.count,
                        g_gamepad ? SDL_GetGamepadName(g_gamepad) : "teclado");
            if (g_emuRunning) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1, 1), "   %.1f img/s   %.0f M instr/s", g_speed.fps, g_speed.mips);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Flechas = cruceta   X = A   Z = B   S = X   A = Y\n"
                                  "Q = L   W = R   1 = ZL   2 = ZR\n"
                                  "Intro = +   Retroceso = -\n"
                                  "T/F/G/H = stick izquierdo   I/J/K/L = stick derecho\n"
                                  "(haz clic fuera de los campos de texto para que el teclado vaya al juego)");
            // Escalar a lo que quepa en la ventana manteniendo 16:9
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            float w = avail.x, h = avail.x * 9.0f / 16.0f;
            if (h > avail.y) { h = avail.y; w = h * 16.0f / 9.0f; }
            if (w > 1 && h > 1) ImGui::Image((ImTextureID)(intptr_t)screenTexture, ImVec2(w, h));
        } else {
            ImGui::TextDisabled("El programa todavia no ha mostrado nada en pantalla.");
        }
        ImGui::End();

        ImGui::Render();
        emuLock.unlock();   // a partir de aqui no se toca la consola: la emulacion sigue

        SDL_SetRenderDrawColor(renderer, 10, 10, 15, 255);
        SDL_RenderClear(renderer);
        // Dibujar en pixeles reales (pantallas con escala != 100%)
        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    if (logoTexture) SDL_DestroyTexture(logoTexture);
    if (screenTexture) SDL_DestroyTexture(screenTexture);
    if (g_gamepad) SDL_CloseGamepad(g_gamepad);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
