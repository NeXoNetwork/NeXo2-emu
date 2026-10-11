#include "audio.hpp"
#include "hle/kernel.hpp"
#include "hle/kernel_objects.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <cstring>

// Formato (comprobado con libnx, services/audout.c):
//   audout:u 0/2  ListAudioOuts(Auto)   -> nombres de 0x100 bytes; u32 numero
//   audout:u 1/3  OpenAudioOut(Auto)    in: u32 frecuencia, u32 canales (libnx pone los
//                 canales en los 16 bits altos: 0x00020000), buffer con el nombre, handle
//                 del proceso. out: u32 frecuencia, u32 canales, u32 formato, u32 estado;
//                 buffer con el nombre; objeto IAudioOut
//   IAudioOut 3/7 AppendAudioOutBuffer(Auto)  in: u64 tag; buffer = AudioOutBuffer
//                 { u64 next; u64 buffer; u64 buffer_size; u64 data_size; u64 data_offset; }
//   IAudioOut 5/8 GetReleasedAudioOutBuffers(Auto)  buffer de u64 (tags); u32 numero
namespace NeXo2::HLE {

namespace {
constexpr u32 PCM_FORMAT_INT16 = 2;
constexpr u32 STATE_STARTED = 0, STATE_STOPPED = 1;
const char DEVICE_NAME[] = "DeviceOut";

// Duracion de 'frames' muestras en ticks del reloj (31,25 MHz)
u64 FramesToTicks(u64 frames, u32 rate) { return frames * 31'250'000ull / (rate ? rate : 48000); }

void Kick(Kernel& k, const std::shared_ptr<AudioOutState>& st);

// El primero de la cola ha terminado de sonar
void Finish(Kernel& k, const std::shared_ptr<AudioOutState>& st) {
    if (st->queue.empty()) { st->head_end = 0; return; }
    const auto b = st->queue.front();
    st->queue.pop_front();
    st->released.push_back(b.tag);
    st->played_frames += b.size / (2ull * st->channels);
    st->last_end = st->head_end;
    st->head_end = 0;
    if (st->event) st->event->signaled = true;
    Kick(k, st);
}

// Si esta en marcha y nada suena, empieza el primero de la cola y programa su final
void Kick(Kernel& k, const std::shared_ptr<AudioOutState>& st) {
    if (!st->started || st->queue.empty() || st->head_end) return;
    const u64 now = k.TicksNow();
    const u64 start = std::max(now, st->last_end);
    const auto& b = st->queue.front();
    st->head_end = start + std::max<u64>(FramesToTicks(b.size / (2ull * st->channels), st->sample_rate), 1);
    std::weak_ptr<AudioOutState> weak = st;
    const u64 gen = st->generation;
    k.ScheduleAt(st->head_end, [&k, weak, gen] {
        if (auto s = weak.lock(); s && s->generation == gen) Finish(k, s);
    });
}

// Manda las muestras del buffer al sonido del PC (estereo, 48 kHz)
void Play(IpcContext& ctx, const AudioOutState& st, const AudioOutState::Buffer& b) {
    const size_t frames = size_t(b.size / (2ull * st.channels));
    if (!frames || frames > 48000 * 4) return;
    std::vector<s16> in(frames * st.channels);
    ctx.GetMemory().ReadBytes(b.address, in.data(), in.size() * 2);
    std::vector<s16> out;
    const u32 rate = st.sample_rate ? st.sample_rate : 48000;
    const size_t out_frames = rate == 48000 ? frames : size_t(u64(frames) * 48000 / rate);
    out.resize(out_frames * 2);
    for (size_t i = 0; i < out_frames; ++i) {
        const size_t src = rate == 48000 ? i : size_t(u64(i) * rate / 48000);
        const s16* f = &in[src * st.channels];
        const float l = f[0] * st.volume, r = (st.channels > 1 ? f[1] : f[0]) * st.volume;
        out[i * 2] = s16(std::clamp(l, -32768.0f, 32767.0f));
        out[i * 2 + 1] = s16(std::clamp(r, -32768.0f, 32767.0f));
    }
    ctx.GetKernel().PushAudio(out.data(), out_frames);
}

void Open(IpcContext& ctx) {
    const u32 rate_in = ctx.Pop<u32>();
    const u32 ch_in = ctx.Pop<u32>();
    u32 channels = ch_in & 0xFFFF ? ch_in & 0xFFFF : ch_in >> 16;
    if (channels != 1 && channels != 2) channels = 2;   // 6 canales: se tratan como 2
    const u32 rate = 48000;                             // la consola solo da 48 kHz
    if (rate_in && rate_in != rate)
        Common::Logger::Log(Common::Logger::Level::Warning, "[audout:u] frecuencia " + std::to_string(rate_in) + " pedida; se usa 48000");
    ctx.WriteBuffer(DEVICE_NAME, sizeof(DEVICE_NAME), 0);
    ctx.Push<u32>(rate);
    ctx.Push<u32>(channels);
    ctx.Push<u32>(PCM_FORMAT_INT16);
    ctx.Push<u32>(STATE_STOPPED);
    ctx.PushInterface(std::make_shared<AudioOut>(rate, channels));
    Common::Logger::Log(Common::Logger::Level::Info, "[audout:u] salida abierta: 48000 Hz, " + std::to_string(channels) + " canales");
    ctx.SetResult(Result::Success);
}

void List(IpcContext& ctx) {
    char name[0x100] = {};
    std::memcpy(name, DEVICE_NAME, sizeof(DEVICE_NAME));
    const bool fits = ctx.GetWriteBufferSize(0) >= sizeof(DEVICE_NAME);
    if (fits) ctx.WriteBuffer(name, std::min<size_t>(sizeof(name), size_t(ctx.GetWriteBufferSize(0))), 0);
    ctx.Push<u32>(fits ? 1 : 0);
    ctx.SetResult(Result::Success);
}
} // namespace

AudioOutManager::AudioOutManager() : ServiceObject("audout:u") {
    RegisterCommand(0, "ListAudioOuts", List);
    RegisterCommand(1, "OpenAudioOut", Open);
    RegisterCommand(2, "ListAudioOutsAuto", List);
    RegisterCommand(3, "OpenAudioOutAuto", Open);
}

AudioOut::AudioOut(u32 sample_rate, u32 channels)
    : ServiceObject("IAudioOut"), m_state(std::make_shared<AudioOutState>()) {
    m_state->sample_rate = sample_rate;
    m_state->channels = channels;
    m_state->event = std::make_shared<KEvent>("audout: buffer soltado");
    RegisterCommand(0, "GetAudioOutState", [this](IpcContext& ctx) {
        ctx.Push<u32>(m_state->started ? STATE_STARTED : STATE_STOPPED);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "Start", [this](IpcContext& ctx) {
        auto& st = *m_state;
        if (!st.started) {
            st.started = true;
            st.last_end = 0;
            for (const auto& b : st.queue) Play(ctx, st, b);
            Kick(ctx.GetKernel(), m_state);
        }
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(2, "Stop", [this](IpcContext& ctx) {
        m_state->started = false;
        m_state->head_end = 0;
        ++m_state->generation;   // el aviso programado ya no vale
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(3, "AppendAudioOutBuffer", [this](IpcContext& ctx) { Append(ctx); });
    RegisterCommand(7, "AppendAudioOutBufferAuto", [this](IpcContext& ctx) { Append(ctx); });
    RegisterCommand(4, "RegisterBufferEvent", [this](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().Handles().Create(m_state->event));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(5, "GetReleasedAudioOutBuffers", [this](IpcContext& ctx) { GetReleased(ctx); });
    RegisterCommand(8, "GetReleasedAudioOutBuffersAuto", [this](IpcContext& ctx) { GetReleased(ctx); });
    RegisterCommand(6, "ContainsAudioOutBuffer", [this](IpcContext& ctx) {
        const u64 tag = ctx.Pop<u64>();
        bool found = std::any_of(m_state->queue.begin(), m_state->queue.end(), [&](const auto& b) { return b.tag == tag; });
        found = found || std::find(m_state->released.begin(), m_state->released.end(), tag) != m_state->released.end();
        ctx.Push<u8>(found ? 1 : 0);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(9, "GetAudioOutBufferCount", [this](IpcContext& ctx) {
        ctx.Push<u32>(u32(m_state->queue.size()));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(10, "GetAudioOutPlayedSampleCount", [this](IpcContext& ctx) {
        ctx.Push<u64>(m_state->played_frames);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(11, "FlushAudioOutBuffers", [this](IpcContext& ctx) {
        auto& st = *m_state;
        const bool any = !st.queue.empty();
        for (const auto& b : st.queue) st.released.push_back(b.tag);
        st.queue.clear();
        st.head_end = 0;
        ++st.generation;
        if (any) st.event->signaled = true;
        ctx.Push<u8>(any ? 1 : 0);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(12, "SetAudioOutVolume", [this](IpcContext& ctx) {
        float v; const u32 raw = ctx.Pop<u32>(); std::memcpy(&v, &raw, 4);
        m_state->volume = std::clamp(v, 0.0f, 2.0f);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(13, "GetAudioOutVolume", [this](IpcContext& ctx) {
        u32 raw; std::memcpy(&raw, &m_state->volume, 4);
        ctx.Push<u32>(raw);
        ctx.SetResult(Result::Success);
    });
}

void AudioOut::Append(IpcContext& ctx) {
    const u64 tag = ctx.Pop<u64>();
    const auto raw = ctx.ReadBuffer(0);
    struct { u64 next, buffer, buffer_size, data_size, data_offset; } desc{};
    std::memcpy(&desc, raw.data(), std::min(raw.size(), sizeof(desc)));
    AudioOutState::Buffer b;
    b.tag = tag;
    b.address = desc.buffer + desc.data_offset;
    b.size = std::min(desc.data_size, desc.buffer_size ? desc.buffer_size : desc.data_size);
    m_state->queue.push_back(b);
    if (m_state->started) {
        Play(ctx, *m_state, b);
        Kick(ctx.GetKernel(), m_state);
    }
    ctx.SetResult(Result::Success);
}

void AudioOut::GetReleased(IpcContext& ctx) {
    auto& rel = m_state->released;
    const size_t max = size_t(ctx.GetWriteBufferSize(0) / 8);
    const size_t n = std::min(max, rel.size());
    std::vector<u64> tags(std::max<size_t>(max, 1), 0);
    std::copy(rel.begin(), rel.begin() + std::ptrdiff_t(n), tags.begin());
    rel.erase(rel.begin(), rel.begin() + std::ptrdiff_t(n));
    if (max) ctx.WriteBuffer(tags.data(), max * 8, 0);
    if (rel.empty()) m_state->event->signaled = false;
    ctx.Push<u32>(u32(n));
    ctx.SetResult(Result::Success);
}

} // namespace NeXo2::HLE
