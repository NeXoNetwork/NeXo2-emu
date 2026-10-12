#include "audren.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include "hle/kernel.hpp"
#include "hle/kernel_objects.hpp"
#include "common/logger.hpp"

// Formatos de RequestUpdate (comprobados con libnx: audren.h y audio/driver.c)
//   Entrada: cabecera (0x40) | comportamiento | mempools (0x20 c/u) | canales de voz (0x70) |
//            voces (0x170) | efectos (0xC0) | splitters | mezclas (0x930) | sinks (0x140) | rendimiento
//   Salida:  cabecera (0x40) | mempools (0x10) | voces (0x10) | efectos (0x10) | sinks (0x20) |
//            rendimiento (0x10) | comportamiento (0xB0)
namespace NeXo2::HLE {

using Common::Logger;

namespace {
constexpr u32 UNUSED_MIX = 0x7FFFFFFF;
constexpr u32 FRESH = 0xFFFFFFFF;   // Voice::offset: el buffer actual aun no ha empezado
constexpr size_t MEMPOOL_IN = 0x20, CHANNEL_IN = 0x70, VOICE_IN = 0x170, EFFECT_IN = 0xC0, MIX_IN = 0x930, SINK_IN = 0x140;

template <typename T> T Get(const std::vector<u8>& b, size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, &b[off], sizeof(T));
    return v;
}
template <typename T> void Put(std::vector<u8>& b, size_t off, T v) {
    if (off + sizeof(T) <= b.size()) std::memcpy(&b[off], &v, sizeof(T));
}

using State = AudioRendererState;

// Un buffer terminado: al siguiente
void FinishWaveBuf(State::Voice& v) {
    v.wb[v.index].valid = false;
    ++v.consumed;
    v.index = (v.index + 1) & 3;
    v.offset = FRESH;
}

s16 Clamp16(s32 x) { return s16(std::clamp(x, -32768, 32767)); }

// La siguiente muestra (todos los canales) de la voz. false si no hay nada que tocar.
bool NextFrame(Core::Memory& mem, State::Voice& v, std::array<float, 6>& out) {
    for (int guard = 0; guard < 8; ++guard) {
        auto& w = v.wb[v.index];
        if (!w.valid) return false;
        if (v.offset == FRESH) {
            v.offset = u32(std::max(w.start, 0));
            if (v.format == 6 && w.context) {   // ADPCM: historia guardada al empezar el buffer
                v.adpcm_scale = mem.Read<u8>(w.context);
                v.hist1 = mem.Read<s16>(w.context + 2);
                v.hist2 = mem.Read<s16>(w.context + 4);
            }
        }
        if (!w.address || w.end <= w.start || v.offset >= u32(w.end)) { FinishWaveBuf(v); continue; }
        const u32 ch = std::clamp<u32>(v.channels, 1, 6);
        const u64 s = v.offset;
        switch (v.format) {
            case 1:
                for (u32 c = 0; c < ch; ++c) out[c] = float(s8(mem.Read<u8>(w.address + s * ch + c))) * 256.0f;
                break;
            case 3:
                for (u32 c = 0; c < ch; ++c) {
                    const u64 a = w.address + (s * ch + c) * 3;
                    const s32 x = s32((u32(mem.Read<u8>(a)) << 8) | (u32(mem.Read<u8>(a + 1)) << 16) | (u32(mem.Read<u8>(a + 2)) << 24)) >> 8;
                    out[c] = float(x) / 256.0f;
                }
                break;
            case 4:
                for (u32 c = 0; c < ch; ++c) out[c] = float(s32(mem.Read<u32>(w.address + (s * ch + c) * 4))) / 65536.0f;
                break;
            case 5:
                for (u32 c = 0; c < ch; ++c) {
                    float f; const u32 raw = mem.Read<u32>(w.address + (s * ch + c) * 4); std::memcpy(&f, &raw, 4);
                    out[c] = f * 32767.0f;
                }
                break;
            case 6: {   // ADPCM: tramas de 8 bytes = cabecera + 14 muestras de 4 bits (mono)
                const u64 frame = s / 14, idx = s % 14;
                if (idx == 0) v.adpcm_scale = mem.Read<u8>(w.address + frame * 8);
                const u8 byte = mem.Read<u8>(w.address + frame * 8 + 1 + idx / 2);
                s32 nib = (idx & 1) ? (byte & 0xF) : (byte >> 4);
                if (nib >= 8) nib -= 16;
                const u32 pred = (v.adpcm_scale >> 4) & 7;
                const s32 scale = 1 << (v.adpcm_scale & 0xF);
                const s32 c1 = v.coefs[pred * 2], c2 = v.coefs[pred * 2 + 1];
                const s32 x = ((nib * scale) << 11) + 1024 + c1 * v.hist1 + c2 * v.hist2;
                const s16 sample = Clamp16(x >> 11);
                v.hist2 = v.hist1;
                v.hist1 = sample;
                out[0] = float(sample);
                for (u32 c = 1; c < 6; ++c) out[c] = out[0];
                break;
            }
            default:   // PCM 16 bits
                for (u32 c = 0; c < ch; ++c) out[c] = float(s16(mem.Read<u16>(w.address + (s * ch + c) * 2)));
                break;
        }
        ++v.offset;
        if (v.offset >= u32(w.end)) {
            if (w.loop) v.offset = FRESH;   // vuelta al principio del mismo buffer
            else FinishWaveBuf(v);
        }
        return true;
    }
    return false;
}

float Biquad(State::Voice::Biquad& f, float s[2], float x) {
    const float y = x * f.b[0] + s[0];
    s[0] = x * f.b[1] - y * f.a[0] + s[1];
    s[1] = x * f.b[2] - y * f.a[1];
    return y;
}
} // namespace

// ============================================================================
//  RequestUpdate
// ============================================================================

void AudioRenderer::Update(Core::Memory& mem, State& st, const std::vector<u8>& in, std::vector<u8>& out) {
    const u32 rev = Get<u32>(in, 0);
    const u32 behavior_sz = Get<u32>(in, 4), mempools_sz = Get<u32>(in, 8), voices_sz = Get<u32>(in, 0xC);
    const u32 channels_sz = Get<u32>(in, 0x10), effects_sz = Get<u32>(in, 0x14), mixes_sz = Get<u32>(in, 0x18);
    const u32 sinks_sz = Get<u32>(in, 0x1C), perf_sz = Get<u32>(in, 0x20), splitter_sz = Get<u32>(in, 0x24);
    st.revision = rev;
    const size_t n_mempools = mempools_sz / MEMPOOL_IN, n_channels = channels_sz / CHANNEL_IN;
    const size_t n_voices = voices_sz / VOICE_IN, n_effects = effects_sz / EFFECT_IN;
    const size_t n_mixes = mixes_sz / MIX_IN, n_sinks = sinks_sz / SINK_IN;
    if (st.voices.size() < n_voices) st.voices.resize(n_voices);
    if (st.channels.size() < n_channels) { st.channels.resize(n_channels); st.channel_used.resize(n_channels); }
    if (st.mixes.size() < n_mixes) st.mixes.resize(n_mixes);
    if (st.sinks.size() < n_sinks) st.sinks.resize(n_sinks);
    if (st.mempool_states.size() < n_mempools) st.mempool_states.resize(n_mempools);

    // Salida: cabecera + mempools + voces + efectos + sinks + rendimiento + comportamiento
    const size_t out_mempools = n_mempools * 0x10, out_voices = n_voices * 0x10, out_effects = n_effects * 0x10;
    const size_t out_sinks = n_sinks * 0x20, out_perf = 0x10, out_behavior = 0xB0;
    out.assign(0x40 + out_mempools + out_voices + out_effects + out_sinks + out_perf + out_behavior, 0);
    Put<u32>(out, 0, rev);
    Put<u32>(out, 4, u32(out_behavior));
    Put<u32>(out, 8, u32(out_mempools));
    Put<u32>(out, 0xC, u32(out_voices));
    Put<u32>(out, 0x14, u32(out_effects));
    Put<u32>(out, 0x1C, u32(out_sinks));
    Put<u32>(out, 0x20, u32(out_perf));
    Put<u32>(out, 0x3C, u32(out.size()));
    size_t o_mempool = 0x40, o_voice = o_mempool + out_mempools, o_effect = o_voice + out_voices;
    size_t o_sink = o_effect + out_effects;

    size_t p = 0x40 + behavior_sz;
    // --- Mempools: pedir enganchar/soltar -> hecho ---
    for (size_t i = 0; i < n_mempools; ++i, p += MEMPOOL_IN) {
        const u32 state = Get<u32>(in, p + 0x10);
        u32 now = 0;
        if (state == 4) now = 5;        // RequestAttach -> Attached
        else if (state == 2) now = 3;   // RequestDetach -> Detached
        st.mempool_states[i] = now ? now : state;
        Put<u32>(out, o_mempool + i * 0x10, now);
    }
    // --- Canales de voz (volumen hacia cada buffer de mezcla) ---
    for (size_t i = 0; i < n_channels; ++i, p += CHANNEL_IN) {
        const u32 id = Get<u32>(in, p);
        const size_t slot = id < n_channels ? id : i;
        for (int b = 0; b < 24; ++b) st.channels[slot][size_t(b)] = Get<float>(in, p + 4 + 4 * size_t(b));
        st.channel_used[slot] = Get<u8>(in, p + 0x64) != 0;
    }
    // --- Voces ---
    for (size_t i = 0; i < n_voices; ++i, p += VOICE_IN) {
        const u32 id = Get<u32>(in, p);
        const size_t slot = id < n_voices ? id : i;
        auto& v = st.voices[slot];
        const bool is_new = Get<u8>(in, p + 8), used = Get<u8>(in, p + 9);
        if (is_new) {
            v = State::Voice{};
            v.index = u32(Get<s16>(in, p + 0x40)) & 3;
            v.offset = FRESH;
        }
        v.used = used;
        if (used) {
            v.state = Get<u8>(in, p + 0xA);
            v.format = Get<u8>(in, p + 0xB);
            v.sample_rate = Get<u32>(in, p + 0xC);
            v.channels = std::clamp<u32>(Get<u32>(in, p + 0x18), 1, 6);
            v.pitch = Get<float>(in, p + 0x1C);
            v.volume = Get<float>(in, p + 0x20);
            for (int b = 0; b < 2; ++b) {
                const size_t q = p + 0x24 + 0xC * size_t(b);
                v.biquad[b].on = Get<u8>(in, q) != 0;
                for (int k = 0; k < 3; ++k) v.biquad[b].b[k] = float(Get<s16>(in, q + 2 + 2 * size_t(k))) / 16384.0f;
                for (int k = 0; k < 2; ++k) v.biquad[b].a[k] = float(Get<s16>(in, q + 8 + 2 * size_t(k))) / 16384.0f;
            }
            const u64 extra = Get<u64>(in, p + 0x48), extra_sz = Get<u64>(in, p + 0x50);
            if (v.format == 6 && extra && extra_sz >= 32)
                for (int k = 0; k < 16; ++k) v.coefs[k] = s16(mem.Read<u16>(extra + 2 * u64(k)));
            v.dest_mix = Get<u32>(in, p + 0x58);
            for (int c = 0; c < 6; ++c) v.channel_ids[size_t(c)] = Get<u32>(in, p + 0x140 + 4 * size_t(c));
            // Buffers en cola: 'count' a partir de 'head' (anillo de 4). Los no enviados aun son nuevos.
            const u32 count = std::min<u32>(Get<u32>(in, p + 0x3C), 4), head = u32(Get<s16>(in, p + 0x40)) & 3;
            for (int b = 0; b < 4; ++b) {
                const size_t q = p + 0x60 + 0x38 * size_t(b);
                if (((u32(b) - head) & 3) >= count) continue;   // hueco libre del anillo
                if (Get<u8>(in, q + 0x1A)) continue;            // ya enviado antes: el servidor ya lo tiene
                auto& w = v.wb[size_t(b)];
                w.address = Get<u64>(in, q);
                w.size = Get<u64>(in, q + 8);
                w.start = Get<s32>(in, q + 0x10);
                w.end = Get<s32>(in, q + 0x14);
                w.loop = Get<u8>(in, q + 0x18) != 0;
                w.end_of_stream = Get<u8>(in, q + 0x19) != 0;
                w.context = Get<u64>(in, q + 0x20);
                w.valid = true;
            }
        }
        Put<u64>(out, o_voice + slot * 0x10, v.played);
        Put<u32>(out, o_voice + slot * 0x10 + 8, v.consumed);
    }
    p += effects_sz;     // efectos: aun no (el sonido pasa tal cual)
    p += splitter_sz;    // splitters: se mezcla directo a la mezcla final
    // --- Mezclas ---
    for (size_t i = 0; i < n_mixes; ++i, p += MIX_IN) {
        auto& m = st.mixes[i];
        m.volume = Get<float>(in, p);
        m.buffers = std::min<u32>(Get<u32>(in, p + 8), 24);
        m.used = Get<u8>(in, p + 0xC) != 0;
        m.id = Get<u32>(in, p + 0x10);
        m.matrix.resize(24 * 24);
        for (size_t k = 0; k < 24 * 24; ++k) m.matrix[k] = Get<float>(in, p + 0x24 + 4 * k);
        m.dest = Get<u32>(in, p + 0x924);
    }
    // Donde empieza cada mezcla en los buffers: la final (id 0) primero, luego el resto
    u32 offset = 0;
    for (auto& m : st.mixes) if (m.used && m.id == 0) { m.offset = 0; offset = m.buffers; }
    for (auto& m : st.mixes) if (m.used && m.id != 0) { m.offset = offset; offset += m.buffers; }
    st.mix_buffer_count = std::max(st.mix_buffer_count, offset);
    // --- Sinks ---
    for (size_t i = 0; i < n_sinks; ++i, p += SINK_IN) {
        auto& s = st.sinks[i];
        s.type = Get<u8>(in, p);
        s.used = Get<u8>(in, p + 1) != 0;
        if (s.type == 1) {
            s.input_count = std::min<u32>(Get<u32>(in, p + 0x120), 6);
            for (int k = 0; k < 6; ++k) s.inputs[k] = Get<u8>(in, p + 0x124 + size_t(k));
        } else if (s.type == 2) {
            s.buffer = Get<u64>(in, p + 0x20);
            s.buffer_size = Get<u32>(in, p + 0x28);
            s.input_count = std::min<u32>(Get<u32>(in, p + 0x2C), 6);
            for (int k = 0; k < 6; ++k) s.inputs[k] = Get<u8>(in, p + 0x3C + size_t(k));
        }
        Put<u32>(out, o_sink + i * 0x20, s.written);
    }
    (void)perf_sz;
    // NEXO2_AUDREN_DEBUG=1: resumen de cada RequestUpdate en el registro (para depurar juegos)
    static const bool debug = std::getenv("NEXO2_AUDREN_DEBUG") != nullptr;
    if (debug) {
        std::string line = "[audren] update REV" + std::string(1, char(rev >> 24)) + " tamanos: mempools " + std::to_string(mempools_sz) +
                           " canales " + std::to_string(channels_sz) + " voces " + std::to_string(voices_sz) + " efectos " +
                           std::to_string(effects_sz) + " mezclas " + std::to_string(mixes_sz) + " sinks " + std::to_string(sinks_sz) +
                           " splitter " + std::to_string(splitter_sz) + " total " + std::to_string(in.size()) + ": ";
        int shown = 0;
        for (size_t i = 0; i < st.voices.size() && shown < 6; ++i) {
            const auto& v = st.voices[i];
            if (!v.used) continue;
            ++shown;
            int valid = 0;
            for (const auto& w : v.wb) valid += w.valid;
            line += "voz " + std::to_string(i) + " (estado " + std::to_string(v.state) + ", formato " + std::to_string(v.format) +
                    ", " + std::to_string(v.sample_rate) + " Hz, " + std::to_string(v.channels) + " ch, vol " +
                    std::to_string(v.volume) + ", mezcla " + std::to_string(v.dest_mix) + ", buffers " + std::to_string(valid) +
                    ", canal0 " + std::to_string(v.channel_ids[0]) + ") ";
        }
        for (const auto& m : st.mixes) if (m.used) line += "| mezcla " + std::to_string(m.id) + " x" + std::to_string(m.buffers) + " vol " + std::to_string(m.volume) + " ";
        for (const auto& k : st.sinks) if (k.used) line += "| sink tipo " + std::to_string(k.type) + " entradas " + std::to_string(k.input_count) + " ";
        Logger::Log(Logger::Level::Info, line);
    }
}

// ============================================================================
//  Mezcla de un trozo (sample_count muestras)
// ============================================================================

void AudioRenderer::RenderFrame(Kernel& kernel, Core::Memory& mem, State& st, std::vector<s16>* capture) {
    const u32 N = std::max<u32>(st.sample_count, 1);
    const u32 nbuf = std::max<u32>(st.mix_buffer_count, 2);
    std::vector<float> buf(size_t(nbuf) * N, 0.0f);
    auto find_mix = [&](u32 id) -> const State::Mix* {
        for (const auto& m : st.mixes) if (m.used && m.id == id) return &m;
        return nullptr;
    };
    const State::Mix* final_mix = find_mix(0);

    // --- Voces ---
    for (auto& v : st.voices) {
        if (!v.used || v.state != 0) continue;
        const State::Mix* dest = v.dest_mix != UNUSED_MIX ? find_mix(v.dest_mix) : nullptr;
        if (!dest) dest = final_mix;
        if (!dest) continue;
        const u32 ch = std::clamp<u32>(v.channels, 1, 6);
        const double step = double(v.sample_rate ? v.sample_rate : st.sample_rate) / double(st.sample_rate) *
                            double(v.pitch > 0 ? v.pitch : 1.0f);
        const size_t needed = size_t(v.frac + double(N - 1) * step) + 2;
        while (v.fifo.size() < needed) {
            std::array<float, 6> f{};
            const bool real = NextFrame(mem, v, f);
            if (!real) f = {};   // sin datos: silencio
            for (u32 c = 0; c < ch; ++c)
                for (int b = 0; b < 2; ++b)
                    if (v.biquad[b].on) f[c] = Biquad(v.biquad[b], v.bq_state[c][b], f[c]);
            v.fifo.push_back(f);
            v.fifo_real.push_back(real);
        }
        for (u32 i = 0; i < N; ++i) {
            const double pos = v.frac + double(i) * step;
            const size_t idx = size_t(pos);
            const float t = float(pos - double(idx));
            for (u32 c = 0; c < ch; ++c) {
                const float s = (v.fifo[idx][c] * (1 - t) + v.fifo[idx + 1][c] * t) * v.volume;
                const u32 cid = v.channel_ids[c];
                if (cid >= st.channels.size()) continue;
                const auto& vol = st.channels[cid];
                for (u32 b = 0; b < dest->buffers; ++b)
                    if (vol[b] != 0.0f && dest->offset + b < nbuf) buf[size_t(dest->offset + b) * N + i] += s * vol[b];
            }
        }
        const double end = v.frac + double(N) * step;
        const size_t pop = std::min(size_t(end), v.fifo.size() - 1);
        for (size_t k = 0; k < pop; ++k) if (v.fifo_real[k]) ++v.played;   // muestras tocadas de verdad
        v.fifo.erase(v.fifo.begin(), v.fifo.begin() + std::ptrdiff_t(pop));
        v.fifo_real.erase(v.fifo_real.begin(), v.fifo_real.begin() + std::ptrdiff_t(pop));
        v.frac = end - double(pop);
    }

    // --- Submezclas, de la mas lejana a la final ---
    std::vector<std::pair<int, const State::Mix*>> order;
    for (const auto& m : st.mixes) {
        if (!m.used || m.id == 0) continue;
        int depth = 0;
        for (const State::Mix* x = &m; x && x->id != 0 && depth < 32; x = find_mix(x->dest)) ++depth;
        order.push_back({depth, &m});
    }
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [depth, m] : order) {
        const State::Mix* dest = find_mix(m->dest);
        if (!dest) continue;
        for (u32 s = 0; s < m->buffers; ++s)
            for (u32 d = 0; d < dest->buffers; ++d) {
                const float k = m->volume * m->matrix[size_t(s) * 24 + d];
                if (k == 0.0f || m->offset + s >= nbuf || dest->offset + d >= nbuf) continue;
                const float* src = &buf[size_t(m->offset + s) * N];
                float* dst = &buf[size_t(dest->offset + d) * N];
                for (u32 i = 0; i < N; ++i) dst[i] += src[i] * k;
            }
    }
    if (final_mix && final_mix->volume != 1.0f)
        for (u32 b = 0; b < final_mix->buffers && b < nbuf; ++b)
            for (u32 i = 0; i < N; ++i) buf[size_t(b) * N + i] *= final_mix->volume;

    // --- Salidas ---
    std::vector<s16> stereo;
    for (auto& s : st.sinks) {
        if (!s.used || !s.input_count) continue;
        auto in = [&](u32 k, u32 i) { const u32 b = s.inputs[k]; return b < nbuf ? buf[size_t(b) * N + i] : 0.0f; };
        if (s.type == 1 && stereo.empty()) {
            stereo.resize(size_t(N) * 2);
            for (u32 i = 0; i < N; ++i) {
                float l, r;
                if (s.input_count >= 6) {   // FL FR C LFE BL BR -> estereo
                    l = in(0, i) + 0.707f * in(2, i) + 0.707f * in(4, i);
                    r = in(1, i) + 0.707f * in(2, i) + 0.707f * in(5, i);
                } else if (s.input_count >= 2) {
                    l = in(0, i); r = in(1, i);
                } else {
                    l = r = in(0, i);
                }
                stereo[i * 2] = Clamp16(s32(std::lround(l)));
                stereo[i * 2 + 1] = Clamp16(s32(std::lround(r)));
            }
        } else if (s.type == 2 && s.buffer && s.buffer_size) {
            for (u32 i = 0; i < N; ++i)
                for (u32 k = 0; k < s.input_count; ++k) {
                    mem.Write<u16>(s.buffer + s.written, u16(Clamp16(s32(std::lround(in(k, i))))));
                    s.written = (s.written + 2) % s.buffer_size;
                }
        }
    }
    if (!stereo.empty()) {
        if (st.sample_rate != 48000 && st.sample_rate) {   // 32 kHz -> 48 kHz
            const size_t out_n = size_t(N) * 48000 / st.sample_rate;
            std::vector<s16> up(out_n * 2);
            for (size_t i = 0; i < out_n; ++i) {
                const double pos = double(i) * st.sample_rate / 48000.0;
                const size_t a = std::min<size_t>(size_t(pos), N - 1), b = std::min<size_t>(a + 1, N - 1);
                const double t = pos - double(a);
                for (int c = 0; c < 2; ++c)
                    up[i * 2 + size_t(c)] = s16(stereo[a * 2 + size_t(c)] * (1 - t) + stereo[b * 2 + size_t(c)] * t);
            }
            stereo.swap(up);
        }
        if (capture) capture->insert(capture->end(), stereo.begin(), stereo.end());
        else kernel.PushAudio(stereo.data(), stereo.size() / 2);
    }
    ++st.frames_rendered;
}

// ============================================================================
//  Servicios
// ============================================================================

namespace {
// Tamano de la memoria de trabajo que pide el programa (un valor holgado, como la consola)
u64 WorkBufferSize(const std::vector<u8>& p) {
    const u64 rate = Get<u32>(p, 0), samples = Get<u32>(p, 4), mixbufs = Get<u32>(p, 8), submixes = Get<u32>(p, 0xC);
    const u64 voices = Get<u32>(p, 0x10), sinks = Get<u32>(p, 0x14), effects = Get<u32>(p, 0x18);
    (void)rate;
    u64 size = 0x4000;
    size += (mixbufs + 6) * samples * 4 * 2;
    size += voices * 0x400 + (submixes + 1) * 0x1000 + sinks * 0x200 + effects * 0x2000;
    return (size + 0xFFF) & ~u64(0xFFF);
}

void Open(IpcContext& ctx) {
    std::vector<u8> p(0x34);
    ctx.PopBytes(p.data(), p.size());
    const u32 rate = Get<u32>(p, 0), samples = Get<u32>(p, 4), mixbufs = Get<u32>(p, 8);
    const u32 submixes = Get<u32>(p, 0xC), voices = Get<u32>(p, 0x10), sinks = Get<u32>(p, 0x14);
    const u32 effects = Get<u32>(p, 0x18), revision = Get<u32>(p, 0x30);
    ctx.PushInterface(std::make_shared<AudioRenderer>(rate ? rate : 48000, samples ? samples : 240, mixbufs, voices, sinks,
                                                      submixes, effects, revision));
    Logger::Log(Logger::Level::Info, "[audren:u] Renderizador abierto: " + std::to_string(rate) + " Hz, " +
                std::to_string(voices) + " voces, " + std::to_string(mixbufs) + " buffers de mezcla");
    ctx.SetResult(Result::Success);
}
} // namespace

AudioRendererManager::AudioRendererManager() : ServiceObject("audren:u") {
    RegisterCommand(0, "OpenAudioRenderer", Open);
    RegisterCommand(1, "GetWorkBufferSize", [](IpcContext& ctx) {
        std::vector<u8> p(0x34);
        ctx.PopBytes(p.data(), p.size());
        ctx.Push<u64>(WorkBufferSize(p));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(2, "GetAudioDeviceService", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<AudioDevice>());
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(4, "GetAudioDeviceServiceWithRevisionInfo", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<AudioDevice>());
        ctx.SetResult(Result::Success);
    });
}

AudioRenderer::AudioRenderer(u32 sample_rate, u32 sample_count, u32 mix_buffers, u32 voices, u32 sinks, u32 submixes,
                             u32 effects, u32 revision)
    : ServiceObject("IAudioRenderer"), m_state(std::make_shared<State>()) {
    (void)submixes; (void)effects; (void)sinks; (void)voices;
    m_state->sample_rate = sample_rate;
    m_state->sample_count = sample_count;
    m_state->mix_buffer_count = mix_buffers;
    m_state->revision = revision;
    m_state->event = std::make_shared<KEvent>("audren: trozo mezclado");
    RegisterCommand(0, "GetSampleRate", [this](IpcContext& ctx) { ctx.Push<u32>(m_state->sample_rate); ctx.SetResult(Result::Success); });
    RegisterCommand(1, "GetSampleCount", [this](IpcContext& ctx) { ctx.Push<u32>(m_state->sample_count); ctx.SetResult(Result::Success); });
    RegisterCommand(2, "GetMixBufferCount", [this](IpcContext& ctx) { ctx.Push<u32>(m_state->mix_buffer_count); ctx.SetResult(Result::Success); });
    RegisterCommand(3, "GetState", [this](IpcContext& ctx) { ctx.Push<u32>(m_state->started ? 0 : 1); ctx.SetResult(Result::Success); });
    auto update = [this](IpcContext& ctx) {
        std::vector<u8> out;
        Update(ctx.GetMemory(), *m_state, ctx.ReadBuffer(0), out);
        ctx.WriteBuffer(out.data(), out.size(), 0);
        ctx.SetResult(Result::Success);
    };
    RegisterCommand(4, "RequestUpdate", update);
    RegisterCommand(10, "RequestUpdateAuto", update);
    RegisterCommand(5, "Start", [this](IpcContext& ctx) {
        auto st = m_state;
        if (st->started) { ctx.SetResult(Result::Success); return; }
        st->started = true;
        const u64 gen = ++st->generation;
        // Cada trozo (5 ms a 48 kHz): mezclar, mandar al PC y avisar al programa
        const u64 period = std::max<u64>(u64(st->sample_count) * 31'250'000ull / std::max<u32>(st->sample_rate, 1), 1);
        Kernel& k = ctx.GetKernel();
        Core::Memory& mem = ctx.GetMemory();
        std::weak_ptr<State> weak = st;
        auto tick = std::make_shared<std::function<void(u64)>>();
        std::weak_ptr<std::function<void(u64)>> weak_tick = tick;
        *tick = [&k, &mem, weak, gen, period, weak_tick](u64 when) {
            auto s = weak.lock();
            if (!s || !s->started || s->generation != gen) return;
            // Si el temporizador llega tarde (la CPU emulada iba a rafagas), se mezclan los trozos
            // que faltan: el PC recibe siempre 48000 muestras por segundo
            const u64 now = k.TicksNow();
            u64 next = when;
            int frames = 0;
            do {
                RenderFrame(k, mem, *s);
                next += period;
            } while (next <= now && ++frames < 40);
            if (next + period < now) next = now + period;   // muy atrasado: se tira el resto
            s->event->signaled = true;
            if (auto t = weak_tick.lock()) k.ScheduleAt(next, [t, next] { (*t)(next); });
        };
        const u64 first = k.TicksNow() + period;
        st->tick = tick;   // vive mientras el renderizador este en marcha
        k.ScheduleAt(first, [tick, first] { (*tick)(first); });
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(6, "Stop", [this](IpcContext& ctx) {
        m_state->started = false;
        ++m_state->generation;
        m_state->tick.reset();
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(7, "QuerySystemEvent", [this](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().Handles().Create(m_state->event));
        ctx.SetResult(Result::Success);
    });
    RegisterStub(8, "SetRenderingTimeLimit");
    RegisterCommand(9, "GetRenderingTimeLimit", [](IpcContext& ctx) { ctx.Push<u32>(100); ctx.SetResult(Result::Success); });
    RegisterStub(11, "ExecuteAudioRendererRendering");
}

AudioDevice::AudioDevice() : ServiceObject("IAudioDevice") {
    auto list = [](IpcContext& ctx) {
        char name[0x100] = "AudioTvOutput";
        const size_t room = size_t(ctx.GetWriteBufferSize(0) / 0x100);
        if (room) ctx.WriteBuffer(name, 0x100, 0);
        ctx.Push<u32>(room ? 1 : 0);
        ctx.SetResult(Result::Success);
    };
    auto active = [](IpcContext& ctx) {
        char name[0x100] = "AudioTvOutput";
        ctx.WriteBuffer(name, std::min<size_t>(0x100, size_t(ctx.GetWriteBufferSize(0))), 0);
        ctx.SetResult(Result::Success);
    };
    auto volume = [](IpcContext& ctx) { ctx.Push<float>(1.0f); ctx.SetResult(Result::Success); };
    auto event = [](IpcContext& ctx) {
        ctx.PushCopyHandle(ctx.GetKernel().CreateEvent("dispositivo de audio", true));
        ctx.SetResult(Result::Success);
    };
    RegisterCommand(0, "ListAudioDeviceName", list);
    RegisterStub(1, "SetAudioDeviceOutputVolume");
    RegisterCommand(2, "GetAudioDeviceOutputVolume", volume);
    RegisterCommand(3, "GetActiveAudioDeviceName", active);
    RegisterCommand(4, "QueryAudioDeviceSystemEvent", event);
    RegisterCommand(5, "GetActiveChannelCount", [](IpcContext& ctx) { ctx.Push<u32>(2); ctx.SetResult(Result::Success); });
    RegisterCommand(6, "ListAudioDeviceNameAuto", list);
    RegisterStub(7, "SetAudioDeviceOutputVolumeAuto");
    RegisterCommand(8, "GetAudioDeviceOutputVolumeAuto", volume);
    RegisterCommand(10, "GetActiveAudioDeviceNameAuto", active);
    RegisterCommand(11, "QueryAudioDeviceInputEvent", event);
    RegisterCommand(12, "QueryAudioDeviceOutputEvent", event);
    RegisterCommand(13, "GetActiveAudioOutputDeviceName", active);
}

} // namespace NeXo2::HLE
