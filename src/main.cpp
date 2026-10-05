#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "arm64/jit_ballistic.hpp"
#include "system.hpp"
#include "common/logger.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using NeXo2::Core::BallisticJit;
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

// Demo para el JIT (3x MOVZ), alineado a 16 bytes.
alignas(16) static const uint32_t g_demo_program[] = {
    0xD2824680u, 0xD2800841u, 0xD29FE002u,
};

static void LoadDemo(System& sys) {
    sys.LoadRawProgram(g_program, sizeof(g_program) / sizeof(g_program[0]), PROG_BASE);
}

// Ejecucion continua: en cada fotograma de la interfaz la CPU emulada corre
// durante unos milisegundos. Asi la ventana sigue respondiendo mientras tanto.
static bool g_emuRunning = false;
static unsigned long long g_runInstructions = 0;   // instrucciones desde que se pulso Run

static void SetRunning(System& sys, bool run) {
    if (run == g_emuRunning) return;
    auto& cpu = sys.GetCpu();
    if (run && cpu.IsHalted()) return;   // parada: hay que reiniciar o cargar otro programa
    g_emuRunning = run;
    if (run) g_runInstructions = 0;
    NeXo2::Common::Logger::Log(NeXo2::Common::Logger::Level::Info,
        run ? std::string("[UI] Run") : "[UI] Pausa (" + std::to_string(g_runInstructions) + " instrucciones)");
}

// Ejecuta trozos de 1M instrucciones hasta gastar 'budget_ms' o hasta que la CPU se pare.
static void RunSlice(System& sys, double budget_ms) {
    auto& cpu = sys.GetCpu();
    const auto start = std::chrono::steady_clock::now();
    while (!cpu.IsHalted()) {
        g_runInstructions += cpu.Run(1'000'000);
        const std::chrono::duration<double, std::milli> spent = std::chrono::steady_clock::now() - start;
        if (spent.count() >= budget_ms) break;
    }
    if (cpu.IsHalted()) {
        g_emuRunning = false;
        NeXo2::Common::Logger::Log(NeXo2::Common::Logger::Level::Info,
            "[UI] Run: " + std::to_string(g_runInstructions) + " instrucciones ejecutadas. " + cpu.GetHaltReason());
    }
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
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
    BallisticJit jit;

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
    int lastIrCount = -1;

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

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                LogUi("Clic raton en (" + std::to_string((int)event.button.x) + ", " +
                      std::to_string((int)event.button.y) + ")");
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5) LogUi("Tecla F5");
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

        // La CPU emulada corre ~12 ms por fotograma (el resto es para la interfaz)
        if (g_emuRunning) RunSlice(sys, 12.0);

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
        Tick("JIT: Ballistic (IR front-end)", jit.IsReady());
        Tick("SDL3 Graphics Driver", true);
        Tick("Vulkan Core", false);

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

        ImGui::Separator();
        ImGui::Text("Ballistic JIT (traduce ARM64 -> IR; sin ejecucion todavia)");
        if (ImGui::Button("Traducir demo (3x MOVZ)")) {
            const std::size_t n = sizeof(g_demo_program) / sizeof(g_demo_program[0]);
            lastIrCount = jit.TranslateFlat(g_demo_program, n);
        }
        ImGui::SameLine();
        if (lastIrCount >= 0) ImGui::Text("IR generada: %d instrucciones", lastIrCount);
        else                  ImGui::TextDisabled("(sin traducir aun)");

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
            ImGui::Text("Imagenes: %llu", (unsigned long long)frame.count);
            // Escalar a lo que quepa en la ventana manteniendo 16:9
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            float w = avail.x, h = avail.x * 9.0f / 16.0f;
            if (h > avail.y) { h = avail.y; w = h * 16.0f / 9.0f; }
            if (w > 1 && h > 1) ImGui::Image((ImTextureID)(intptr_t)screenTexture, ImVec2(w, h));
        } else {
            ImGui::TextDisabled("El programa todavia no ha mostrado nada en pantalla.");
        }
        ImGui::End();

        SDL_SetRenderDrawColor(renderer, 10, 10, 15, 255);
        SDL_RenderClear(renderer);
        ImGui::Render();
        // Dibujar en pixeles reales (pantallas con escala != 100%)
        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    if (logoTexture) SDL_DestroyTexture(logoTexture);
    if (screenTexture) SDL_DestroyTexture(screenTexture);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
