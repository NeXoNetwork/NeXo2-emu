#include "emu_host.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdlib>
#include "common/logger.hpp"
#include "hle/kernel.hpp"
#include "loader/package.hpp"
#include "video_core/gpu.hpp"

namespace NeXo2::QtUi {

using Common::Logger;

namespace {
void Log(const std::string& s) { Logger::Log(Logger::Level::Info, "[UI] " + s); }
bool EnvOff(const char* name) { const char* e = std::getenv(name); return e && e[0] == '0'; }
std::atomic<int> g_volume{100};
std::atomic<bool> g_audioOn{true};
} // namespace

// ---------------------------------------------------------------------------
//  Hilo de emulacion (el mismo esquema que NeXo2.exe, ver main.cpp)
// ---------------------------------------------------------------------------
class EmuHost::Runner {
public:
    Runner(Core::System& sys, std::atomic<bool>& running) : m_sys(sys), m_running(running), m_thread([this] { Loop(); }) {}
    ~Runner() {
        m_quit = true;
        m_thread.join();
        m_sys.GetKernel().StopCores();
    }
    std::unique_lock<std::mutex> LockForUi() {
        m_uiWaiting = true;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_uiWaiting = false;
        return lock;
    }
    void Restart() { ++m_generation; }
    std::string halt;   // motivo de la ultima parada (lo lee la interfaz con el candado)

private:
    bool RunMulticore() {
        auto& k = m_sys.GetKernel();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (k.CoresHalted()) {
                k.StopCores();
                if (m_sys.ContinueAfterExit()) {
                    ++m_generation;
                } else if (m_running) {
                    m_running = false;
                    halt = m_sys.GetCpu().GetHaltReason();
                    Log("Parada (multinucleo): " + halt);
                }
            } else if (m_running && !k.CoresRunning()) {
                k.StartCores();
                if (!k.CoresRunning()) return false;
            } else if (!m_running && k.CoresRunning()) {
                k.StopCores();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return true;
    }

    void Loop() {
        while (!m_quit) {
            if (m_sys.GetKernel().IsMulticore() && RunMulticore()) continue;
            if (!m_running || m_uiWaiting) {
                std::this_thread::sleep_for(std::chrono::microseconds(m_running ? 0 : 2000));
                continue;
            }
            u64 sleep_ns = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_running) continue;
                auto& cpu = m_sys.GetCpu();
                m_sys.Run(200'000);
                sleep_ns = Pace(cpu);
                if (cpu.IsHalted() && m_sys.ContinueAfterExit()) {
                    ++m_generation;
                } else if (cpu.IsHalted()) {
                    m_running = false;
                    halt = cpu.GetHaltReason();
                    Log("Parada: " + halt);
                }
            }
            if (sleep_ns) std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        }
    }

    // Reloj emulado = reloj real (un solo hilo; con varios nucleos ya lo es)
    u64 Pace(Core::Interpreter& cpu) {
        using Clock = std::chrono::steady_clock;
        const u64 ticks = cpu.GetTicks();
        if (!m_paceValid || ticks < m_paceLast || m_paceGen != m_generation) {
            m_paceValid = true;
            m_paceGen = m_generation;
            m_paceStart = Clock::now();
            m_paceTicks0 = m_paceLast = ticks;
            return 0;
        }
        constexpr double TICKS_PER_NS = double(Core::Interpreter::TICK_FREQUENCY) / 1e9;
        const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_paceStart).count());
        const u64 wall = m_paceTicks0 + u64(ns * TICKS_PER_NS);
        u64 sleep_ns = 0;
        if (ticks < wall) cpu.AddTicks(wall - ticks);
        else if (ticks > wall + u64(2e6 * TICKS_PER_NS)) sleep_ns = u64(std::min(double(ticks - wall) / TICKS_PER_NS, 20e6));
        m_paceLast = cpu.GetTicks();
        return sleep_ns;
    }

    Core::System& m_sys;
    std::atomic<bool>& m_running;
    std::atomic<u64> m_generation{0};
    bool m_paceValid = false;
    u64 m_paceGen = 0, m_paceTicks0 = 0, m_paceLast = 0;
    std::chrono::steady_clock::time_point m_paceStart;
    std::mutex m_mutex;
    std::atomic<bool> m_quit{false}, m_uiWaiting{false};
    std::thread m_thread;
};

// ---------------------------------------------------------------------------

EmuHost::EmuHost(std::string base_dir) : m_base(std::move(base_dir)) {
    m_ini = m_base + "nexo2.ini";
    m_settings.Load(m_ini);
    if (m_settings.gpu_threads > 0 && !std::getenv("NEXO2_GPU_THREADS"))
        SDL_setenv_unsafe("NEXO2_GPU_THREADS", std::to_string(m_settings.gpu_threads).c_str(), 1);
    // SDL solo para el sonido y los mandos (la ventana es de Qt)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);

    const std::string sdmc = m_base + "sdmc";
    m_sys.GetKernel().SetSdmcRoot(Frontend::Utf8ToPath(sdmc));
    std::error_code ec;
    std::filesystem::create_directories(Frontend::Utf8ToPath(sdmc) / "switch", ec);
    std::filesystem::create_directories(Frontend::Utf8ToPath(m_base + "games"), ec);
    std::filesystem::create_directories(Frontend::Utf8ToPath(m_base + "keys"), ec);

    ApplySettings();
    OpenAudio();
    m_runner = std::make_unique<Runner>(m_sys, m_running);
}

EmuHost::~EmuHost() {
    m_running = false;
    m_runner.reset();
    m_sys.GetKernel().SetAudioSink(nullptr);
    if (m_audio) SDL_DestroyAudioStream(m_audio);
    if (m_pad) SDL_CloseGamepad(m_pad);
    SDL_Quit();
}

void EmuHost::SaveSettings() { m_settings.Save(m_ini); }

std::unique_lock<std::mutex> EmuHost::LockEmu() { return m_runner->LockForUi(); }

void EmuHost::ApplySettings() {
    std::unique_lock<std::mutex> emu;
    if (m_runner) emu = LockEmu();
    auto& k = m_sys.GetKernel();
    if (k.CoresRunning()) k.StopCores();   // al volver a Run se arrancan con los ajustes nuevos
    m_sys.GetCpu().SetJitEnabled(m_settings.jit);
    m_sys.GetCpu().SetDecodeCacheEnabled(m_settings.decode_cache);
    k.SetMulticore(m_settings.multicore && !EnvOff("NEXO2_MULTICORE"));
    k.GetGpu().SetAsync(m_settings.gpu_async && !EnvOff("NEXO2_GPU_ASYNC"));
    bool docked = m_settings.docked == 1;
    if (const char* d = std::getenv("NEXO2_DOCKED")) docked = std::string(d) == "1";
    k.SetDocked(docked);
    bool want_vk = m_settings.renderer == 1;
    if (const char* r = std::getenv("NEXO2_RENDERER")) want_vk = std::string(r) == "vulkan";
    std::string err;
    if (!k.GetGpu().SetRenderer(want_vk ? GPU::RendererKind::Vulkan : GPU::RendererKind::Software, &err))
        Log("Vulkan no disponible (" + err + "): se dibuja por software");
    Log("Dibujo: " + k.GetGpu().RendererName());
    g_volume = m_settings.volume;
    g_audioOn = m_settings.audio;
}

void EmuHost::ApplySettingsLight() {
    g_volume = m_settings.volume;
    g_audioOn = m_settings.audio;
}

std::string EmuHost::RendererName() { return m_sys.GetKernel().GetGpu().RendererName(); }

void EmuHost::OpenAudio() {
    if (EnvOff("NEXO2_AUDIO") || !SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
    m_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!m_audio) { Log(std::string("Sin sonido: ") + SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(m_audio);
    SDL_AudioStream* stream = m_audio;
    // ~60 ms en cola; el ritmo se ajusta un 2 % como mucho (igual que NeXo2.exe)
    m_sys.GetKernel().SetAudioSink([stream](const s16* samples, size_t frames) {
        if (!g_audioOn) return;
        constexpr int BYTES_PER_SEC = 48000 * 4, TARGET = BYTES_PER_SEC * 60 / 1000;
        const int queued = SDL_GetAudioStreamQueued(stream);
        if (queued > BYTES_PER_SEC / 2) SDL_ClearAudioStream(stream);
        SDL_SetAudioStreamFrequencyRatio(stream, std::clamp(1.0f + (float(queued) / float(TARGET) - 1.0f) * 0.02f, 0.98f, 1.02f));
        const int vol = g_volume;
        if (vol >= 100) { SDL_PutAudioStreamData(stream, samples, int(frames * 4)); return; }
        std::vector<s16> scaled(samples, samples + frames * 2);
        for (auto& v : scaled) v = s16(int(v) * vol / 100);
        SDL_PutAudioStreamData(stream, scaled.data(), int(frames * 4));
    });
    Log("Sonido: 48000 Hz estereo");
}

std::vector<std::filesystem::path> EmuHost::LibraryDirs() const {
    std::vector<std::filesystem::path> dirs{Frontend::Utf8ToPath(m_base + "sdmc") / "switch", Frontend::Utf8ToPath(m_base + "games")};
    for (const auto& d : m_settings.game_dirs) dirs.push_back(Frontend::Utf8ToPath(d));
    return dirs;
}

std::vector<Frontend::GameEntry> EmuHost::ScanLibrary() {
    Loader::KeySet keys;
    std::string keys_dir;
    const bool have = Loader::LoadKeys(m_sys.KeyDirs(), keys, keys_dir);
    auto games = Frontend::ScanGames(LibraryDirs(), have ? &keys : nullptr);
    Log("Biblioteca: " + std::to_string(games.size()) + " juegos");
    return games;
}

bool EmuHost::Launch(const std::string& path, std::string& error) {
    Stop();
    ApplySettings();
    auto emu = LockEmu();
    auto kl = m_sys.GetKernel().Lock();
    std::string ext = Frontend::PathToUtf8(Frontend::Utf8ToPath(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const bool game = ext != ".nro" && Core::System::IsGameFolder(path);
    if (!(game ? m_sys.LoadGame(path) : m_sys.LoadNroFile(path))) {
        error = m_sys.GetLastError();
        m_inGame = false;
        return false;
    }
    const auto* info = m_sys.GetNroInfo();
    m_title = game ? m_sys.GetProgramName()
            : info && !info->title.empty() ? info->title : Frontend::PathToUtf8(Frontend::Utf8ToPath(path).stem());
    m_runner->halt.clear();
    m_runner->Restart();
    if (m_audio) SDL_ClearAudioStream(m_audio);
    m_inGame = true;
    m_running = true;
    m_speedFrames = 0;
    Log("Jugar: " + path);
    return true;
}

void EmuHost::Stop() {
    if (!m_inGame && !m_running) return;
    m_running = false;
    {
        auto emu = LockEmu();
        m_sys.GetKernel().StopCores();
    }
    if (m_audio) SDL_ClearAudioStream(m_audio);
    m_inGame = false;
    Log("Detener");
}

void EmuHost::SetPaused(bool paused) {
    if (!m_inGame) return;
    if (!paused && m_sys.GetCpu().IsHalted()) return;
    m_running = !paused;
    if (paused) {
        auto emu = LockEmu();
        m_sys.GetKernel().StopCores();
        if (m_audio) SDL_ClearAudioStream(m_audio);
    } else {
        m_runner->Restart();
    }
    Log(paused ? "Pausa" : "Continuar");
}

std::string EmuHost::HaltReason() {
    auto emu = LockEmu();
    return m_runner->halt;
}

bool EmuHost::GrabFrame(u64& seen, std::vector<u32>& rgba, u32& width, u32& height) {
    auto& display = m_sys.GetKernel().GetDisplay();
    auto lock = display.LockFrame();
    const auto& f = display.Frame();
    if (f.count == 0 || f.count == seen || !f.width || !f.height || f.rgba.size() < size_t(f.width) * f.height) return false;
    seen = f.count;
    width = f.width;
    height = f.height;
    rgba = f.rgba;
    return true;
}

void EmuHost::UpdateGamepad() {
    SDL_UpdateGamepads();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !m_pad) {
            m_pad = SDL_OpenGamepad(e.gdevice.which);
            if (m_pad) Log(std::string("Mando conectado: ") + SDL_GetGamepadName(m_pad));
        } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && m_pad && SDL_GetGamepadID(m_pad) == e.gdevice.which) {
            SDL_CloseGamepad(m_pad);
            m_pad = nullptr;
            Log("Mando desconectado");
        }
    }
}

std::string EmuHost::GamepadName() const {
    const char* n = m_pad ? SDL_GetGamepadName(m_pad) : nullptr;
    return n ? n : "";
}

void EmuHost::Tick(const HLE::PadInput& keyboard) {
    using namespace HLE;
    UpdateGamepad();
    PadInput pad = keyboard;
    if (m_pad) {
        const struct { SDL_GamepadButton b; u64 button; } map[] = {
            {SDL_GAMEPAD_BUTTON_EAST, NpadButton::A},  {SDL_GAMEPAD_BUTTON_SOUTH, NpadButton::B},
            {SDL_GAMEPAD_BUTTON_NORTH, NpadButton::X}, {SDL_GAMEPAD_BUTTON_WEST, NpadButton::Y},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, NpadButton::L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, NpadButton::R},
            {SDL_GAMEPAD_BUTTON_START, NpadButton::Plus}, {SDL_GAMEPAD_BUTTON_BACK, NpadButton::Minus},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, NpadButton::StickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, NpadButton::StickR},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, NpadButton::Up},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, NpadButton::Down},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, NpadButton::Left}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, NpadButton::Right},
        };
        for (const auto& m : map) if (SDL_GetGamepadButton(m_pad, m.b)) pad.buttons |= m.button;
        if (SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) pad.buttons |= NpadButton::ZL;
        if (SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) pad.buttons |= NpadButton::ZR;
        auto axis = [](Sint16 v, bool invert) {
            const int x = invert ? -int(v) : int(v);
            return (x > -6000 && x < 6000) ? 0 : std::clamp(x, -STICK_MAX, STICK_MAX);
        };
        const s32 lx = axis(SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_LEFTX), false);
        const s32 ly = axis(SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_LEFTY), true);
        const s32 rx = axis(SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_RIGHTX), false);
        const s32 ry = axis(SDL_GetGamepadAxis(m_pad, SDL_GAMEPAD_AXIS_RIGHTY), true);
        if (lx || ly) { pad.lx = lx; pad.ly = ly; }
        if (rx || ry) { pad.rx = rx; pad.ry = ry; }
    }
    if (m_running) {
        auto emu = LockEmu();
        auto kl = m_sys.GetKernel().Lock();
        m_sys.GetKernel().SetPadInput(pad);
    }

    // Velocidad, cada segundo
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - m_speedTime).count();
    if (secs >= 1.0) {
        u64 frames = 0;
        { auto l = m_sys.GetKernel().GetDisplay().LockFrame(); frames = m_sys.GetKernel().GetDisplay().Frame().count; }
        u64 instr = 0;
        for (int c = 0; c < HLE::NUM_CORES; ++c)
            if (auto* cpu = m_sys.GetKernel().CoreCpu(c)) instr += cpu->GetInstructionCount();
        if (m_running && frames >= m_speedFrames && instr >= m_speedInstr) {
            m_fps = double(frames - m_speedFrames) / secs;
            m_mips = double(instr - m_speedInstr) / secs / 1e6;
        } else {
            m_fps = m_mips = 0;
        }
        m_speedFrames = frames;
        m_speedInstr = instr;
        m_speedTime = now;
    }
}

} // namespace NeXo2::QtUi
