// Tests del renderizador de audio (audren:u): RequestUpdate con el formato de libnx y la
// mezcla de cada trozo de 5 ms (voces PCM y ADPCM, volumenes por canal, buffers consumidos).
#include <cstring>
#include <vector>
#include "test_framework.hpp"
#include "system.hpp"
#include "hle/services/audren.hpp"

using namespace NeXo2;

namespace {
using Bytes = std::vector<u8>;
template <typename T> void Put(Bytes& b, size_t off, T v) { std::memcpy(&b[off], &v, sizeof(T)); }

constexpr u64 PCM = 0x2000'0000, ADPCM = 0x2000'4000, COEFS = 0x2000'5000;

struct VoiceDesc {
    u8 format = 2;
    u32 rate = 48000;
    bool is_new = true;
    bool sent = false;           // los buffers ya se mandaron antes
    std::vector<std::pair<u64, s32>> bufs;   // direccion, numero de muestras
};

// Entrada de RequestUpdate con 1 mempool, 1 canal, 1 voz, mezcla final de 2 buffers y un sink
// with_mixer: la mezcla final tiene 4 buffers, la voz va a 0/1, un efecto BufferMixer los suma
// en 2/3 y la salida lee 2/3 (como hacen juegos de verdad)
Bytes Input(const VoiceDesc& vd, bool with_mixer = false) {
    const u32 behavior = 0x10, mempools = 0x20, channels = 0x70, voices = 0x170, mixes = 0x930, sinks = 0x140, perf = 0x10;
    const u32 effects = with_mixer ? 0xC0 : 0;
    Bytes in(0x40 + behavior + mempools + channels + voices + effects + mixes + sinks + perf, 0);
    Put<u32>(in, 0, 0x36564552);   // REV6
    Put<u32>(in, 4, behavior); Put<u32>(in, 8, mempools); Put<u32>(in, 0xC, voices); Put<u32>(in, 0x10, channels);
    Put<u32>(in, 0x14, effects); Put<u32>(in, 0x18, mixes); Put<u32>(in, 0x1C, sinks); Put<u32>(in, 0x20, perf); Put<u32>(in, 0x3C, u32(in.size()));
    size_t p = 0x40 + behavior;
    Put<u64>(in, p, PCM); Put<u64>(in, p + 8, 0x10000); Put<u32>(in, p + 0x10, 4);   // RequestAttach
    p += mempools;
    Put<u32>(in, p, 0); Put<float>(in, p + 4, 1.0f); Put<float>(in, p + 8, 0.5f); in[p + 0x64] = 1;   // L = 1, R = 0,5
    p += channels;
    in[p + 8] = vd.is_new; in[p + 9] = 1; in[p + 0xA] = 0; in[p + 0xB] = vd.format;
    Put<u32>(in, p + 0xC, vd.rate); Put<u32>(in, p + 0x18, 1); Put<float>(in, p + 0x1C, 1.0f); Put<float>(in, p + 0x20, 1.0f);
    Put<u32>(in, p + 0x3C, u32(vd.bufs.size()));
    if (vd.format == 6) { Put<u64>(in, p + 0x48, COEFS); Put<u64>(in, p + 0x50, 32); }
    Put<u32>(in, p + 0x58, 0);            // mezcla final
    Put<u32>(in, p + 0x5C, 0xFFFFFFFF);
    for (size_t b = 0; b < vd.bufs.size(); ++b) {
        const size_t q = p + 0x60 + 0x38 * b;
        Put<u64>(in, q, vd.bufs[b].first); Put<u64>(in, q + 8, 0x1000);
        Put<s32>(in, q + 0x10, 0); Put<s32>(in, q + 0x14, vd.bufs[b].second);
        in[q + 0x1A] = vd.sent;
    }
    Put<u32>(in, p + 0x140, 0);           // canal 0
    p += voices;
    if (with_mixer) {                     // BufferMixer: 0 -> 2, 1 -> 3
        in[p] = 1; in[p + 1] = 1; in[p + 2] = 1; Put<u32>(in, p + 4, 0);
        in[p + 0x20] = 0; in[p + 0x21] = 1; in[p + 0x38] = 2; in[p + 0x39] = 3;
        Put<float>(in, p + 0x50, 1.0f); Put<float>(in, p + 0x54, 1.0f); Put<u32>(in, p + 0xB0, 2);
        p += effects;
    }
    Put<float>(in, p, 1.0f); Put<u32>(in, p + 8, with_mixer ? 4 : 2); in[p + 0xC] = 1; Put<u32>(in, p + 0x10, 0);
    Put<u32>(in, p + 0x924, 0x7FFFFFFF);
    p += mixes;
    in[p] = 1; in[p + 1] = 1; Put<u32>(in, p + 0x120, 2);
    in[p + 0x124] = with_mixer ? 2 : 0; in[p + 0x125] = with_mixer ? 3 : 1;
    return in;
}
} // namespace

TEST(Audren_PcmVoiceMixAndBuffersConsumed) {
    Core::System sys;
    auto& mem = sys.GetMemory();
    for (u32 i = 0; i < 480; ++i) mem.Write<u16>(PCM + 2 * i, u16(s16(i * 10)));
    HLE::AudioRendererState st;
    st.sample_rate = 48000; st.sample_count = 240; st.mix_buffer_count = 2;

    Bytes out;
    VoiceDesc vd;
    vd.bufs = {{PCM, 240}, {PCM + 480, 240}};
    HLE::AudioRenderer::Update(mem, st, Input(vd), out);
    CHECK_EQ(u32(out.size()), u32(0x40 + 0x10 + 0x10 + 0x20 + 0x10 + 0xB0));
    u32 v32; std::memcpy(&v32, &out[0x40], 4);
    CHECK_EQ(v32, 5u);                    // mempool: RequestAttach -> Attached
    std::memcpy(&v32, &out[0x3C], 4);
    CHECK_EQ(v32, u32(out.size()));

    std::vector<s16> pcm;
    HLE::AudioRenderer::RenderFrame(sys.GetKernel(), mem, st, &pcm);
    CHECK_EQ(pcm.size(), size_t(480));
    bool ok = true;
    for (u32 i = 0; i < 240; ++i) if (pcm[i * 2] != s16(i * 10) || pcm[i * 2 + 1] != s16(i * 5)) ok = false;
    CHECK(ok);

    // Siguiente RequestUpdate (los buffers ya estan en el servidor): uno consumido, 240 muestras
    vd.is_new = false; vd.sent = true;
    HLE::AudioRenderer::Update(mem, st, Input(vd), out);
    u64 played; std::memcpy(&played, &out[0x50], 8); std::memcpy(&v32, &out[0x58], 4);
    CHECK_EQ(played, 240ULL);
    CHECK_EQ(v32, 1u);
    pcm.clear();
    HLE::AudioRenderer::RenderFrame(sys.GetKernel(), mem, st, &pcm);
    ok = true;
    for (u32 i = 0; i < 239; ++i) if (pcm[i * 2] != s16((240 + i) * 10)) ok = false;
    CHECK(ok);
    pcm.clear();
    HLE::AudioRenderer::RenderFrame(sys.GetKernel(), mem, st, &pcm);   // sin buffers: silencio
    CHECK(pcm[200] == 0);
    HLE::AudioRenderer::Update(mem, st, Input(vd), out);
    std::memcpy(&v32, &out[0x58], 4);
    CHECK_EQ(v32, 2u);
}

TEST(Audren_AdpcmAndPitch) {
    Core::System sys;
    auto& mem = sys.GetMemory();
    // ADPCM con coeficientes 0 y escala 1: cada muestra es su nibble
    const u8 frame[8] = {0x00, 0x12, 0x34, 0x56, 0x70, 0x12, 0x34, 0x56};
    for (int k = 0; k < 40; ++k) mem.WriteBytes(ADPCM + 8 * u64(k), frame, 8);
    for (int k = 0; k < 16; ++k) mem.Write<u16>(COEFS + 2 * u64(k), 0);
    HLE::AudioRendererState st;
    st.sample_rate = 48000; st.sample_count = 240; st.mix_buffer_count = 2;
    Bytes out;
    VoiceDesc vd;
    vd.format = 6;
    vd.bufs = {{ADPCM, 14 * 40}};
    HLE::AudioRenderer::Update(mem, st, Input(vd), out);
    std::vector<s16> pcm;
    HLE::AudioRenderer::RenderFrame(sys.GetKernel(), mem, st, &pcm);
    const s16 expect[14] = {1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6};
    bool ok = true;
    for (u32 i = 0; i < 28; ++i) if (pcm[i * 2] != expect[i % 14]) ok = false;
    CHECK(ok);

    // PCM a 24 kHz: se interpola (cada muestra nueva cae entre dos del original)
    for (u32 i = 0; i < 480; ++i) mem.Write<u16>(PCM + 2 * i, u16(s16(i * 100)));
    HLE::AudioRendererState st2;
    st2.sample_rate = 48000; st2.sample_count = 240; st2.mix_buffer_count = 2;
    VoiceDesc v2;
    v2.rate = 24000;
    v2.bufs = {{PCM, 480}};
    HLE::AudioRenderer::Update(mem, st2, Input(v2), out);
    pcm.clear();
    HLE::AudioRenderer::RenderFrame(sys.GetKernel(), mem, st2, &pcm);
    CHECK(pcm[0] == 0 && pcm[2] == 50 && pcm[4] == 100 && pcm[6] == 150);
}

TEST(Audren_BufferMixerEffect) {
    // Sin el efecto la salida (buffers 2/3) estaria en silencio
    Core::System sys;
    auto& mem = sys.GetMemory();
    for (u32 i = 0; i < 240; ++i) mem.Write<u16>(PCM + 2 * i, u16(s16(i * 10)));
    HLE::AudioRendererState st;
    st.sample_rate = 48000; st.sample_count = 240; st.mix_buffer_count = 4;
    Bytes out;
    VoiceDesc vd;
    vd.bufs = {{PCM, 240}};
    HLE::AudioRenderer::Update(mem, st, Input(vd, true), out);
    std::vector<s16> pcm;
    HLE::AudioRenderer::RenderFrame(sys.GetKernel(), mem, st, &pcm);
    CHECK_EQ(pcm.size(), size_t(480));
    CHECK_EQ(int(pcm[100 * 2]), 1000);    // L: volumen 1
    CHECK_EQ(int(pcm[100 * 2 + 1]), 500); // R: volumen 0,5
}
