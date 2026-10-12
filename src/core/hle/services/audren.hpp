#pragma once
// "audren:u": el renderizador de audio (lo que usan los juegos y OpenAL/SDL en libnx).
//
// El programa describe "voces" (cada una toca buffers de muestras PCM o ADPCM de su
// memoria), mezclas (buffers intermedios con volumenes) y salidas ("sinks"). Cada 5 ms la
// consola mezcla 240 muestras (a 48 kHz) y avisa con un evento; el programa, en
// RequestUpdate, manda el estado nuevo y recibe cuantas muestras y buffers ha consumido
// cada voz. NeXo hace esa mezcla en la CPU con el reloj emulado y manda el resultado al
// sonido del PC (Kernel::PushAudio).
//
// Soportado: PCM 8/16/32 bits, float y ADPCM (con bucles), cambio de tono, volumen,
// filtros biquad, mezclas y submezclas, salida de dispositivo (1, 2 o 6 canales) y
// de buffer circular. Los efectos (reverb, delay, aux...) aun no: el sonido pasa sin ellos.
// Ver docs/07-nexo-internals/audio.md.
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include "hle/service.hpp"

namespace NeXo2::HLE {

class AudioRendererManager final : public ServiceObject {
public:
    AudioRendererManager();
};

struct KEvent;

// Estado del renderizador (compartido con su temporizador)
struct AudioRendererState {
    struct WaveBuf {
        u64 address = 0, size = 0;
        s32 start = 0, end = 0;
        bool loop = false, end_of_stream = false, valid = false;
        u64 context = 0;
    };
    struct Voice {
        bool used = false;
        u32 state = 1;               // 0 tocando, 1 parada, 2 en pausa
        u32 format = 2;              // 1 s8, 2 s16, 3 s24, 4 s32, 5 float, 6 ADPCM
        u32 sample_rate = 48000, channels = 1;
        float pitch = 1, volume = 1;
        u32 dest_mix = 0;
        std::array<u32, 6> channel_ids{};
        struct Biquad { bool on = false; float b[3] = {}, a[2] = {}; } biquad[2];
        float bq_state[6][2][2] = {};
        std::array<WaveBuf, 4> wb{};
        u32 index = 0;               // buffer que suena (0..3)
        u32 offset = 0;              // muestra dentro de ese buffer
        u32 consumed = 0;            // buffers terminados (total, desde que la voz es nueva)
        u64 played = 0;              // muestras tocadas
        // ADPCM
        s16 coefs[16] = {};
        s32 hist1 = 0, hist2 = 0;
        u8 adpcm_scale = 0;
        // Muestras ya decodificadas (para interpolar al cambiar de frecuencia)
        std::deque<std::array<float, 6>> fifo;
        std::deque<bool> fifo_real;  // false = silencio puesto porque no habia datos
        double frac = 0;
    };
    struct Mix {
        bool used = false;
        float volume = 1;
        u32 buffers = 0, id = 0, dest = 0x7FFFFFFF, offset = 0;
        std::vector<float> matrix;   // [origen][destino] 24 x 24
    };
    // Efecto. Solo se aplica BufferMixer (tipo 1: suma buffers de su mezcla en otros; muchos
    // juegos lo usan para llevar el sonido a los buffers que lee la salida). Los demas
    // (reverb, delay, aux...) se saltan: el sonido pasa sin ellos.
    struct Effect {
        u8 type = 0;
        bool enabled = false;
        u32 mix_id = 0;
        s32 order = 0;
        s8 input[24] = {}, output[24] = {};
        float volume[24] = {};
        u32 count = 0;
    };
    struct Sink {
        bool used = false;
        u32 type = 0;                // 1 dispositivo, 2 buffer circular
        u32 input_count = 0;
        u8 inputs[6] = {};
        u64 buffer = 0;              // buffer circular
        u32 buffer_size = 0, written = 0;
    };

    u32 sample_rate = 48000, sample_count = 240, mix_buffer_count = 0;
    u32 revision = 0;
    bool started = false;
    u64 generation = 0;
    std::vector<Voice> voices;
    std::vector<std::array<float, 24>> channels;   // volumenes de cada canal de voz a cada buffer
    std::vector<bool> channel_used;
    std::vector<Mix> mixes;
    std::vector<Sink> sinks;
    std::vector<Effect> effects;
    std::vector<u32> mempool_states;
    std::shared_ptr<KEvent> event;
    u64 frames_rendered = 0;
    std::shared_ptr<std::function<void(u64)>> tick;   // temporizador de cada trozo (mientras suena)
};

class AudioRenderer final : public ServiceObject {
public:
    AudioRenderer(u32 sample_rate, u32 sample_count, u32 mix_buffers, u32 voices, u32 sinks, u32 submixes,
                  u32 effects, u32 revision);
    // Mezcla un trozo (sample_count muestras) y lo manda al sonido del PC. Publico para los tests.
    static void RenderFrame(Kernel& kernel, Core::Memory& memory, AudioRendererState& st, std::vector<s16>* out = nullptr);
    // Procesa la entrada de RequestUpdate y prepara la salida (publico para los tests)
    static void Update(Core::Memory& memory, AudioRendererState& st, const std::vector<u8>& in, std::vector<u8>& out);
private:
    std::shared_ptr<AudioRendererState> m_state;
};

class AudioDevice final : public ServiceObject {
public:
    AudioDevice();
};

} // namespace NeXo2::HLE
