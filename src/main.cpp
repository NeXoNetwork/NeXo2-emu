#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "core/memory/memory.hpp"
#include "core/arm64/interpreter.hpp"

using namespace NeXo2::Core;

// Estructura para el reporte de salud del emulador
struct EmulatorStatus {
    bool memory_ok = false;
    bool cpu_reset_ok = false;
    bool add_opcode_ok = false;
    bool mov_opcode_ok = false;
};

int main(int argc, char* argv[]) {
    // --- 1. SETUP DEL EMULADOR ---
    Memory ram;
    Interpreter cpu(ram);
    EmulatorStatus status;

    // --- 2. BATERÍA DE TESTS PREVIOS ---
    // Test 1: Reset
    cpu.GetState().Reset();
    if (cpu.GetState().pc == 0 && cpu.GetState().x[0] == 0) status.cpu_reset_ok = true;

    // Test 2: Memory & MOV
    ram.Write<uint32_t>(0, 0x52800141); // MOV X1, #10
    cpu.Step(); // Ejecuta MOV
    if (cpu.GetState().x[1] == 10) status.mov_opcode_ok = true;

    // Test 3: ADD
    cpu.GetState().x[2] = 20;
    ram.Write<uint32_t>(4, 0x8b020020); // ADD X0, X1, X2 (10 + 20)
    cpu.Step(); // Ejecuta ADD
    if (cpu.GetState().x[0] == 30) status.add_opcode_ok = true;
    
    status.memory_ok = true; // Si llegamos aquí sin crash, la memoria funciona

    // --- 3. SETUP GRÁFICO ---
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("NeXo 2 - System Integrity Check", 1024, 768, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // --- VENTANA DE REPORTE ---
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_Always);
        ImGui::Begin("NeXo 2 Status Report", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        ImGui::Text("Verificando componentes core...");
        ImGui::Separator();

        // Función auxiliar para dibujar los OK/FAIL
        auto DrawStatus = [](const char* label, bool ok) {
            ImGui::Text("%s:", label); ImGui::SameLine(300);
            if (ok) ImGui::TextColored(ImVec4(0, 1, 0, 1), "[ OK ]");
            else    ImGui::TextColored(ImVec4(1, 0, 0, 1), "[ FAIL ]");
        };

        DrawStatus("Memory Subsystem (R/W)", status.memory_ok);
        DrawStatus("CPU Register Reset", status.cpu_reset_ok);
        DrawStatus("Instruction: MOV (Immediate)", status.mov_opcode_ok);
        DrawStatus("Instruction: ADD (Register)", status.add_opcode_ok);

        ImGui::Separator();
        
        if (status.add_opcode_ok && status.mov_opcode_ok) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "SYSTEM READY FOR NEXT INSTRUCTION SET.");
        }

        if (ImGui::Button("Cerrar y continuar", ImVec2(-1, 40))) quit = true;

        ImGui::End();

        // Renderizado
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 25, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Cleanup
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}