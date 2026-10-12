#pragma once
// La "consola" para la interfaz Qt (NeXo2-Qt.exe): el System emulado, el hilo que lo hace
// correr, el sonido (SDL), el mando del PC (SDL) y los ajustes (nexo2.ini, los mismos que
// NeXo2.exe). Sin nada de Qt: la ventana solo llama a estas funciones.
//
// Candados: el hilo de emulacion coge m_mutex en cada tanda de instrucciones; con varios
// nucleos el kernel tiene el suyo (Kernel::Lock). La interfaz coge los dos (UiLock) para
// cargar, parar o pasar los botones.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "system.hpp"
#include "hle/input.hpp"
#include "frontend/game_list.hpp"
#include "frontend/settings.hpp"

struct SDL_AudioStream;
struct SDL_Gamepad;

namespace NeXo2::QtUi {

class EmuHost {
public:
    explicit EmuHost(std::string base_dir);
    ~EmuHost();

    Core::System& Sys() { return m_sys; }
    Frontend::Settings& Settings() { return m_settings; }
    void SaveSettings();
    // Aplica los ajustes de CPU, GPU, modo de la consola y sonido (al empezar y al cambiarlos)
    void ApplySettings();
    // Solo lo que se puede cambiar con el juego en marcha (volumen, sonido)
    void ApplySettingsLight();
    std::string RendererName();

    // Biblioteca: sdmc/switch, games/ y las carpetas de los ajustes
    std::vector<Frontend::GameEntry> ScanLibrary();
    std::vector<std::filesystem::path> LibraryDirs() const;
    const std::string& BaseDir() const { return m_base; }

    // Juego
    bool Launch(const std::string& path, std::string& error);
    void Stop();
    void SetPaused(bool paused);
    bool Paused() const { return m_inGame && !m_running; }
    bool InGame() const { return m_inGame; }
    const std::string& Title() const { return m_title; }
    std::string HaltReason();   // por que se paro (vacio si sigue)

    // Imagen nueva desde la ultima vez ('seen' = numero de la ultima que se recogio)
    bool GrabFrame(u64& seen, std::vector<u32>& rgba, u32& width, u32& height);

    // Llamar ~120 veces por segundo: lee el mando del PC y pasa los botones al juego
    void Tick(const HLE::PadInput& keyboard);
    std::string GamepadName() const;

    // Velocidad (se recalcula cada segundo en Tick)
    double Fps() const { return m_fps; }
    double Mips() const { return m_mips; }

private:
    class Runner;   // el hilo de emulacion
    std::unique_lock<std::mutex> LockEmu();
    void OpenAudio();
    void UpdateGamepad();

    std::string m_base, m_ini;
    Frontend::Settings m_settings;
    Core::System m_sys;
    std::unique_ptr<Runner> m_runner;
    SDL_AudioStream* m_audio = nullptr;
    SDL_Gamepad* m_pad = nullptr;
    std::atomic<bool> m_running{false};
    bool m_inGame = false;
    std::string m_title;
    double m_fps = 0, m_mips = 0;
    std::chrono::steady_clock::time_point m_speedTime = std::chrono::steady_clock::now();
    u64 m_speedFrames = 0, m_speedInstr = 0;
};

} // namespace NeXo2::QtUi
