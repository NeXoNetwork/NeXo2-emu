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

// Programa ARM64 de ejemplo que se carga en memoria en 0x80000000:
//   MOVZ X0, #0x1234
//   MOVZ X1, #0x0001
//   ADD  X2, X0, #0x10   (X2 = X0 + 0x10 = 0x1244)
//   NOP
static const uint32_t g_program[] = {
    0xD2824680u, 0xD2800021u, 0x91004002u, 0xD503201Fu,
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
        ImGui::Text("X0 = 0x%016llX", (unsigned long long)st.x[0]);
        ImGui::Text("X1 = 0x%016llX", (unsigned long long)st.x[1]);
        ImGui::Text("X2 = 0x%016llX", (unsigned long long)st.x[2]);
        ImGui::Text("Paginas RAM activas: %zu (%.2f MB)",
                    mem.AllocatedPages(), mem.AllocatedBytes() / (1024.0 * 1024.0));

        if (ImGui::Button("Step CPU")) cpu.Step();
        ImGui::SameLine();
        if (ImGui::Button("Reset CPU")) cpu.Reset();
        ImGui::SameLine();
        if (ImGui::Button("Run (4 instr)")) { for (int i = 0; i < 4; ++i) cpu.Step(); }

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
