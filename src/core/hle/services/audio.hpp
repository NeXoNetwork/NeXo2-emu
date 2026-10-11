#pragma once
// "audout:u": salida de sonido sencilla (la que usa audoutInitialize de libnx).
//
// El programa abre una salida (48 kHz, PCM de 16 bits, 1-2 canales), le va dando buffers
// con muestras y espera a que la consola los "suelte" (ya sonados) para rellenarlos otra vez.
// NeXo suelta cada buffer cuando ha pasado su duracion en el reloj emulado (asi el programa
// va a su ritmo) y manda las muestras a la tarjeta de sonido del PC (Kernel::PushAudio).
// Ver docs/07-nexo-internals/audio.md.
#include <deque>
#include <memory>
#include <vector>
#include "hle/service.hpp"

namespace NeXo2::HLE {

class AudioOutManager final : public ServiceObject {
public:
    AudioOutManager();
};

struct KEvent;

// Estado de una salida abierta (compartido con los temporizadores del kernel)
struct AudioOutState {
    struct Buffer { u64 tag = 0, address = 0, size = 0; };
    u32 sample_rate = 48000;
    u32 channels = 2;
    bool started = false;
    float volume = 1.0f;
    std::deque<Buffer> queue;        // por sonar (el primero es el que suena)
    std::vector<u64> released;       // ya sonados, esperando a que el programa los recoja
    u64 head_end = 0;                // tick en que acaba el primero (0 = no programado)
    u64 last_end = 0;                // tick en que acabo el anterior
    u64 generation = 0;              // cambia al parar: los avisos viejos se ignoran
    u64 played_frames = 0;
    std::shared_ptr<KEvent> event;   // "hay buffers soltados"
};

class AudioOut final : public ServiceObject {
public:
    AudioOut(u32 sample_rate, u32 channels);
private:
    void Append(IpcContext& ctx);
    void GetReleased(IpcContext& ctx);
    std::shared_ptr<AudioOutState> m_state;
};

} // namespace NeXo2::HLE
