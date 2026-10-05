#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "arm64/interpreter.hpp"
#include "arm64/jit_ballistic.hpp"
#include "core/memory/memory.hpp"

#include <cstdint>

using NeXo2::Core::BallisticJit;
using NeXo2::Core::Interpreter;
using NeXo2::Core::Memory;

// Programa ARM64 de ejemplo que se carga en memoria en 0x80000000.
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

static void LoadProgram(Memory& mem) {
    for (unsigned i = 0; i < sizeof(g_program) / sizeof(g_program[0]); ++i)
        mem.Write<uint32_t>(PROG_BASE + i * 4, g_program[i]);
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("Error inicializando SDL: %s", SDL_GetError());
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("NeXo 2 | 0.0.0.1", 1280, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    SDL_Texture* logoTexture = nullptr;
    if (SDL_Surface* logoSurface = SDL_LoadBMP("assets/logo.bmp")) {
        logoTexture = SDL_CreateTextureFromSurface(renderer, logoSurface);
        SDL_DestroySurface(logoSurface);
    }

    Memory       mem;
    LoadProgram(mem);        // el programa vive en memoria antes de ejecutar
    Interpreter  cpu(mem);   // PC arranca en 0x80000000
    BallisticJit jit;
    int lastIrCount = -1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Tamano inicial de la ventana (solo la primera vez; luego se puede redimensionar)
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
        ImGui::Begin("NeXo 2 Diagnostics");

        auto Tick = [](const char* label, bool ok) {
            ImGui::TextColored(ok ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1), ok ? "[OK]" : "[XX]");
            ImGui::SameLine(); ImGui::Text("%s", label);
        };

        Tick("ARM64 Interpreter", true);
        Tick("Memory (paged VMM)", mem.IsReady());
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

        if (ImGui::Button("Step CPU")) cpu.Step();
        ImGui::SameLine();
        if (ImGui::Button("Reset CPU")) cpu.Reset();
        ImGui::SameLine();
        if (ImGui::Button("Run (hasta parar)")) cpu.Run(1'000'000);

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
            ImGui::Text("Engine Logo:");
            ImGui::Image((ImTextureID)(intptr_t)logoTexture, ImVec2(150, 75));
        }

        ImGui::End();

        SDL_SetRenderDrawColor(renderer, 10, 10, 15, 255);
        SDL_RenderClear(renderer);
        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    if (logoTexture) SDL_DestroyTexture(logoTexture);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
