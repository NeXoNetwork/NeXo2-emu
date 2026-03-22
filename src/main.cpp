#include <SDL3/SDL.h>
#include <SDL3/SDL_image.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <iostream>
#include <vector>
#include <string>

// --- Configuración de NeXo 2 ---
const int WINDOW_WIDTH = 1280;
const int WINDOW_HEIGHT = 720;

// Función para intentar cargar el logo en diferentes rutas relativas
SDL_Texture* LoadLogo(SDL_Renderer* renderer) {
    // Lista de rutas posibles (desde build/ o desde la raíz)
    std::vector<std::string> paths = {
        "assets/logo.png",
        "../assets/logo.png",
        "../../assets/logo.png"
    };

    SDL_Surface* surface = nullptr;
    for (const auto& path : paths) {
        surface = IMG_Load(path.c_str());
        if (surface) {
            std::cout << "[INFO] Logo cargado desde: " << path << std::endl;
            break;
        }
    }

    if (!surface) {
        std::cerr << "[ERROR] No se pudo encontrar assets/logo.png en ninguna ruta." << std::endl;
        return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

int main(int argc, char* argv[]) {
    // 1. Inicializar SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        std::cerr << "Error SDL_Init: " << SDL_GetError() << std::endl;
        return -1;
    }

    // 2. Inicializar SDL_image para cargar el PNG
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        std::cerr << "Error IMG_Init: " << IMG_GetError() << std::endl;
        return -1;
    }

    // 3. Crear Ventana y Renderer
    SDL_Window* window = SDL_CreateWindow("NeXo 2 Emulator | NX", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL, SDL_RENDERER_ACCELERATED);

    if (!window || !renderer) {
        std::cerr << "Error al crear ventana/renderer: " << SDL_GetError() << std::endl;
        return -1;
    }

    // 4. Configurar ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // 5. Cargar el Logo de NeXo
    SDL_Texture* logoTexture = LoadLogo(renderer);

    // Bucle Principal
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
        }

        // Iniciar Frame de ImGui
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Ventana de Estado (GUI)
        ImGui::Begin("NX 2 - Dashboard");
        ImGui::Text("Emulador NeXo 2 v0.1");
        ImGui::Separator();
        ImGui::Text("Status: Running");
        ImGui::Text("Logo: %s", logoTexture ? "Loaded" : "Missing");
        
        if (ImGui::Button("Reset CPU")) {
            // Aquí irá la lógica de reset
        }
        ImGui::End();

        // --- RENDERIZADO ---
        SDL_SetRenderDrawColor(renderer, 15, 15, 15, 255); // Fondo casi negro
        SDL_RenderClear(renderer);

        // Dibujar el Logo si existe
        if (logoTexture) {
            float scale = 0.5f; // Ajusta el tamaño del logo aquí
            int w, h;
            SDL_QueryTexture(logoTexture, NULL, NULL, &w, &h);
            SDL_FRect destRect = { 20.0f, 20.0f, w * scale, h * scale };
            SDL_RenderTexture(renderer, logoTexture, NULL, &destRect);
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
    }

    // Limpieza
    SDL_DestroyTexture(logoTexture);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();

    return 0;
}