#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

// Includes del núcleo
#include "arm64/interpreter.hpp"
#include "core/memory/memory.hpp" // Se encuentra gracias a target_include_directories

#include <iostream>

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) return -1;

    SDL_Window* window = SDL_CreateWindow("NeXo 2 | 12GB RAM Edition", 1280, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);
    
    // Carga de Logo
    SDL_Surface* logoSurface = SDL_LoadBMP("assets/logo.bmp");
    SDL_Texture* logoTexture = nullptr;
    if (logoSurface) {
        logoTexture = SDL_CreateTextureFromSurface(renderer, logoSurface);
        SDL_DestroySurface(logoSurface);
    }

    // Inicializar Componentes
    Interpreter cpu;
    Memory mem; // Aquí se reservan los 12GB de RAM

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
        Tick("Memory (12GB RAM)", mem.IsReady());
        Tick("SDL3 Graphics Driver", true);
        Tick("Vulkan Core", false);
        
        if (logoTexture) {
            ImGui::Separator();
            ImGui::Text("Engine Logo:");
            ImGui::Image((ImTextureID)logoTexture, ImVec2(150, 75));
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
    SDL_Quit();
    return 0;
}