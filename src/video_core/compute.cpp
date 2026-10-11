#include "compute.hpp"
#include <algorithm>
#include <memory>
#include <vector>
#include "shader.hpp"
#include "texture.hpp"

namespace NeXo2::GPU {

namespace {
inline u64 Iova(u32 hi, u32 lo) { return (u64(hi & 0xFF) << 32) | lo; }

constexpr u32 TSC_POOL = 0x557, TSC_MAX = 0x559;
constexpr u32 TIC_POOL = 0x55D, TIC_MAX = 0x55F;
constexpr u32 BINDLESS_TEXTURE = 0x982;

// Lo que un hilo de compute ve de fuera
class ComputeEnv final : public ShaderEnv {
public:
    u32 ReadConst(u32, u32) override { return 0; }   // (cb_direct: no se llama)
    u32 ReadAttribute(u32) override { return 0; }
    void WriteAttribute(u32, u32) override {}
    float Interpolate(u32, u32) override { return 0.0f; }
    u32 SystemRegister(u32 sr) override {
        const u32 lane = linear & 31;
        switch (sr) {
            case 0x00: return lane;                                          // laneid
            case 0x20: return tid[0] | (tid[1] << 16) | (tid[2] << 26);      // tid (empaquetado)
            case 0x21: return tid[0];
            case 0x22: return tid[1];
            case 0x23: return tid[2];
            case 0x25: return cta[0];
            case 0x26: return cta[1];
            case 0x27: return cta[2];
            case 0x38: return 1u << lane;                                    // eqmask
            case 0x39: return (1u << lane) - 1;                              // ltmask
            case 0x3A: return (lane == 31) ? 0xFFFFFFFFu : ((2u << lane) - 1);   // lemask
            case 0x3B: return ~((lane == 31) ? 0xFFFFFFFFu : ((2u << lane) - 1)); // gtmask
            case 0x3C: return ~((1u << lane) - 1);                           // gemask
            default: return 0;
        }
    }
    void SampleTexture(u32 handle, const TextureRequest& req, u32 out[4]) override {
        TextureRequest r = req;
        if (!r.fetch && !r.explicit_lod && !r.grad) { r.explicit_lod = true; r.lod = 0; }   // sin derivadas
        if (tex) tex->Sample(handle, r, out);
        else { out[0] = out[1] = out[2] = 0; out[3] = 0x3F800000u; }
    }
    void QueryTexture(u32 handle, u32 lod, u32 out[4]) override {
        if (tex) tex->Query(handle, lod, out);
        else out[0] = out[1] = out[2] = out[3] = 0;
    }
    u32 TextureConstbuf() override { return tex_cb; }

    u32 tid[3] = {}, cta[3] = {}, linear = 0;
    TextureSampler* tex = nullptr;
    u32 tex_cb = 0;
};
} // namespace

ComputeQmd ReadComputeQmd(Gpu& gpu, u64 qmd_addr, u64 program_region) {
    u32 w[64] = {};
    gpu.MemoryManager().ReadBlock(qmd_addr, w, sizeof(w));
    ComputeQmd q;
    q.program = program_region + w[8];
    q.grid[0] = w[12];
    q.grid[1] = w[13] & 0xFFFF;
    q.grid[2] = w[13] >> 16;
    q.shared_size = w[17] & 0x3FFFF;
    q.block[0] = w[18] >> 16;
    q.block[1] = w[19] & 0xFFFF;
    q.block[2] = w[19] >> 16;
    const u32 valid = w[20] & 0xFF;
    for (u32 i = 0; i < 8; ++i) {
        const u32 lo = w[29 + 2 * i], hi = w[30 + 2 * i];
        q.cb[i].address = (u64(hi & 0xFF) << 32) | lo;
        q.cb[i].size = hi >> 15;
        q.cb[i].valid = (valid >> i) & 1;
    }
    return q;
}

bool RunCompute(Gpu& gpu, const u32* regs, const ComputeQmd& q, std::string& error) {
    GpuMemoryManager& mm = gpu.MemoryManager();
    const u32 threads = q.block[0] * q.block[1] * q.block[2];
    const u64 groups = u64(q.grid[0]) * q.grid[1] * q.grid[2];
    if (threads == 0 || groups == 0) return true;
    if (threads > 1024) { error = "grupo de mas de 1024 hilos"; return false; }

    auto read = [&mm](u64 a, void* d, size_t n) { mm.ReadBlock(a, d, n); };
    ShaderProgram prog(q.program, read, false);

    // Constbufs: se leen una vez para todo el dispatch
    ComputeEnv env;
    std::array<std::vector<u32>, 8> cbs;
    env.cb_direct = true;
    for (u32 i = 0; i < 8; ++i) {
        if (!q.cb[i].valid || !q.cb[i].size) continue;
        cbs[i].resize((std::min<u32>(q.cb[i].size, 0x10000) + 3) / 4);
        mm.ReadBlock(q.cb[i].address, cbs[i].data(), cbs[i].size() * 4);
        env.cb_data[i] = cbs[i].data();
        env.cb_words[i] = u32(cbs[i].size());
    }
    auto sampler = std::make_unique<TextureSampler>(gpu, Iova(regs[TIC_POOL], regs[TIC_POOL + 1]), regs[TIC_MAX],
                                                    Iova(regs[TSC_POOL], regs[TSC_POOL + 1]), regs[TSC_MAX],
                                                    &gpu.Textures());
    env.tex = sampler.get();
    env.tex_cb = std::min<u32>(regs[BINDLESS_TEXTURE] & 0x1F, 17);
    env.global = &mm;
    std::vector<u8> shared(q.shared_size, 0);
    env.shared = shared.empty() ? nullptr : shared.data();
    env.shared_size = u32(shared.size());

    // Un interprete por hilo del grupo (se reutilizan de un grupo a otro)
    std::vector<ShaderInterpreter> th(threads);
    for (auto& t : th) t.group_mode = true;
    std::vector<u8> state(threads);   // 0 sin empezar, 1 parado en BAR, 2 terminado

    for (u32 cz = 0; cz < q.grid[2]; ++cz)
    for (u32 cy = 0; cy < q.grid[1]; ++cy)
    for (u32 cx = 0; cx < q.grid[0]; ++cx) {
        env.cta[0] = cx; env.cta[1] = cy; env.cta[2] = cz;
        std::fill(shared.begin(), shared.end(), u8(0));
        std::fill(state.begin(), state.end(), u8(0));
        u32 done = 0;
        while (done < threads) {
            bool progress = false;
            for (u32 i = 0; i < threads; ++i) {
                if (state[i] == 2) continue;
                env.linear = i;
                env.tid[0] = i % q.block[0];
                env.tid[1] = (i / q.block[0]) % q.block[1];
                env.tid[2] = i / (q.block[0] * q.block[1]);
                const auto res = state[i] == 0 ? th[i].Run(prog, env) : th[i].Resume(prog, env);
                if (!res.ok) { error = "shader de compute: " + res.error; goto finish; }
                if (res.barrier) state[i] = 1;
                else { state[i] = 2; ++done; }
                progress = true;
            }
            if (!progress) break;
        }
    }
finish:
    if (env.written_hi > env.written_lo) gpu.OnGpuWrite(env.written_lo, env.written_hi - env.written_lo);
    return error.empty();
}

} // namespace NeXo2::GPU
