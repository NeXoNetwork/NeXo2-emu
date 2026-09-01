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

// Pequeno programa ARM64 de ejemplo (3x MOVZ) para probar el front-end del JIT.
// Alineado a 16 bytes porque bal_memory_init_flat lo exige.
alignas(16) static const uint32_t g_demo_program[] = {
    0xD2824680u, // MOVZ X0, #0x1234
    0xD2800841u, // MOVZ X1, #0x0042
    0xD29FE002u, // MOVZ X2, #0xFF00, LSL #16
};

int main(int, char**) {
    // SDL3: SDL_Init devuelve true en exito (ya NO se compara con < 0).
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("Error inicializando SDL: %s", SDL_GetError());
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("NeXo 2 | 0.0.0.1", 1280, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

    // Logo: cargamos un BMP de verdad (un .ico no lo carga SDL_LoadBMP).
    SDL_Texture* logoTexture = nullptr;
    if (SDL_Surface* logoSurface = SDL_LoadBMP("assets/logo.bmp")) {
        logoTexture = SDL_CreateTextureFromSurface(renderer, logoSurface);
        SDL_DestroySurface(logoSurface);
    }

    // Nucleo del emulador.
    Memory       mem;        // memoria por paginas (bajo demanda, no 12 GB de golpe)
    Interpreter  cpu(mem);   // CPU ARM64 (interprete esqueleto)
    BallisticJit jit;        // motor JIT (front-end IR; aun sin backend)
    int lastIrCount = -1;    // resultado de la ultima traduccion de prueba

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
        ImGui::Text("PC = 0x%016llX", (unsigned long long)st.pc);
        ImGui::Text("SP = 0x%016llX", (unsigned long long)st.sp);
        ImGui::Text("X0 = 0x%016llX", (unsigned long long)st.x[0]);
        ImGui::Text("Paginas RAM activas: %zu (%.2f MB)",
                    mem.AllocatedPages(), mem.AllocatedBytes() / (1024.0 * 1024.0));

        if (ImGui::Button("Step CPU")) cpu.Step();
        ImGui::SameLine();
        if (ImGui::Button("Reset CPU")) cpu.Reset();

        ImGui::Separator();
        ImGui::Text("Ballistic JIT (traduce ARM64 -> IR; sin ejecucion todavia)");
        if (ImGui::Button("Traducir demo (3x MOVZ)")) {
            const std::size_t n = sizeof(g_demo_program) / sizeof(g_demo_program[0]);
            lastIrCount = jit.TranslateFlat(g_demo_program, n);
        }
        ImGui::SameLine();
        if (lastIrCount >= 0)
            ImGui::Text("IR generada: %d instrucciones", lastIrCount);
        else
            ImGui::TextDisabled("(sin traducir aun)");

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
