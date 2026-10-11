// Tests del traductor de shaders Maxwell -> SPIR-V (GPU fase 3b, video_core/shader_spirv.hpp).
//
// Los mismos 300 programas al azar que prueban el interprete (tests/generated/shader_fuzz.bin)
// se traducen a SPIR-V y se ejecutan en Vulkan como compute shaders: el resultado tiene que
// ser el mismo que el esperado (calculado en Python) y que el del interprete.
// Sin Vulkan en el PC, solo se comprueba que se traducen.
// NEXO2_SPIRV_DUMP=carpeta guarda cada modulo como .spv (para spirv-val / spirv-dis).
#include "test_framework.hpp"
#include "video_core/shader.hpp"
#include "video_core/shader_spirv.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifdef NEXO2_HAS_VULKAN
#include "video_core/vulkan/vk_device.hpp"
#endif

using namespace NeXo2;
using namespace NeXo2::GPU;

namespace {

float AsF(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }

struct Dksh {
    std::vector<u8> code;
    u32 entry = 0;
    std::vector<u32> cb1;
};
bool ParseDksh(const u8* data, size_t size, Dksh& out) {
    if (size < 24) return false;
    u32 h[6];
    std::memcpy(h, data, 24);
    if (h[0] != 0x48534B44 || h[4] + 64 > size || h[2] > size) return false;
    u32 ph[16];
    std::memcpy(ph, data + h[4], 64);
    out.code.assign(data + h[2], data + size);
    out.entry = ph[1];
    const u32 cb1_off = ph[3], cb1_size = ph[4];
    out.cb1.assign(cb1_size / 4, 0);
    if (cb1_size && cb1_off + cb1_size <= out.code.size()) std::memcpy(out.cb1.data(), &out.code[cb1_off], cb1_size);
    return true;
}
ShaderProgram MakeProgram(const Dksh& d) {
    return ShaderProgram(d.entry, [&d](u64 addr, void* dst, size_t n) {
        std::memset(dst, 0, n);
        if (addr < d.code.size()) std::memcpy(dst, &d.code[addr], std::min<size_t>(n, d.code.size() - addr));
    });
}

struct FuzzCase {
    std::string glsl;
    Dksh dksh;
    bool parsed = false;
    std::vector<u32> ubo;
    struct Out { u32 kind, exp, alt; float tol; } outs[16];
};

std::vector<FuzzCase> LoadFuzz() {
    std::vector<FuzzCase> cases;
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/shader_fuzz.bin", std::ios::binary);
    if (!f) return cases;
    std::vector<u8> blob((std::istreambuf_iterator<char>(f)), {});
    size_t pos = 0;
    auto rd = [&]() { u32 v = 0; if (pos + 4 <= blob.size()) std::memcpy(&v, &blob[pos], 4); pos += 4; return v; };
    if (rd() != 0x4653584Eu || rd() != 1u) return cases;
    const u32 count = rd();
    for (u32 n = 0; n < count && pos < blob.size(); ++n) {
        FuzzCase c;
        const u32 glsl_len = rd();
        c.glsl.assign(reinterpret_cast<const char*>(&blob[pos]), glsl_len);
        pos += (glsl_len + 3) & ~3u;
        const u32 dksh_len = rd();
        c.parsed = ParseDksh(&blob[pos], dksh_len, c.dksh);
        pos += (dksh_len + 3) & ~3u;
        c.ubo.resize(32);
        for (auto& w : c.ubo) w = rd();
        for (auto& o : c.outs) { o.kind = rd(); o.exp = rd(); o.alt = rd(); u32 t = rd(); std::memcpy(&o.tol, &t, 4); }
        cases.push_back(std::move(c));
    }
    return cases;
}

bool OutputOk(const FuzzCase::Out& o, u32 got) {
    if (o.kind == 1) return got == o.exp;
    const float g = AsF(got);
    return got == o.exp || got == o.alt || std::fabs(g - AsF(o.exp)) <= o.tol || std::fabs(g - AsF(o.alt)) <= o.tol;
}

} // namespace

TEST(Spirv_TranslatesFuzzPrograms) {
    auto cases = LoadFuzz();
    CHECK(!cases.empty());
    const char* dump = std::getenv("NEXO2_SPIRV_DUMP");
    u32 ok = 0, n = 0;
    for (auto& c : cases) {
        if (!c.parsed) continue;
        ShaderProgram prog = MakeProgram(c.dksh);
        SpirvOptions opt;
        const SpirvResult r = TranslateToSpirv(prog, opt);
        if (r.ok) {
            ++ok;
            CHECK(r.code.size() > 5 && r.code[0] == 0x07230203u);
            if (dump) {
                std::ofstream o(std::string(dump) + "/fuzz" + std::to_string(n) + ".spv", std::ios::binary);
                o.write(reinterpret_cast<const char*>(r.code.data()), std::streamsize(r.code.size() * 4));
            }
        } else if (ok + 5 > n) {
            std::printf("    prueba %u: %s\n", n, r.error.c_str());
        }
        ++n;
    }
    std::printf("    %u de %u programas traducidos a SPIR-V\n", ok, n);
    CHECK_EQ(ok, n);
}

#ifdef NEXO2_HAS_VULKAN
namespace {
namespace VK = NeXo2::GPU::Vulkan;
VK::Device* SpirvDevice() {
    static VK::Device dev;
    static bool tried = false;
    if (!tried) { tried = true; dev.Init(std::getenv("NEXO2_VK_VALIDATION") != nullptr); }
    return dev.Ok() ? &dev : nullptr;
}

// Ejecuta un modulo de modo ComputeTest con estos constbufs y devuelve attr_out
bool RunCompute(VK::Device& dev, const SpirvResult& r, const std::map<u32, std::vector<u32>>& cbufs, std::vector<u32>& out) {
    VkDevice d = dev.Handle();
    std::vector<u32> bindings;
    for (u32 i = 0; i < 18; ++i) if (r.cbuf_mask & (1u << i)) bindings.push_back(i);
    bindings.push_back(SpirvBinding::ATTR_IN);
    bindings.push_back(SpirvBinding::ATTR_OUT);

    std::vector<VK::Buffer> bufs;
    for (u32 bnd : bindings) {
        VK::Buffer buf = dev.CreateBuffer(0x10000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (!buf.mapped) return false;
        std::memset(buf.mapped, 0, 0x10000);
        auto it = cbufs.find(bnd);
        if (it != cbufs.end() && bnd < 18)
            std::memcpy(buf.mapped, it->second.data(), std::min<size_t>(it->second.size() * 4, 0x10000));
        bufs.push_back(buf);
    }
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = r.code.size() * 4;
    smi.pCode = r.code.data();
    VkShaderModule module;
    if (vkCreateShaderModule(d, &smi, nullptr, &module) != VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayoutBinding> lb(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        lb[i] = {};
        lb[i].binding = bindings[i];
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = u32(lb.size());
    dli.pBindings = lb.data();
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(d, &dli, nullptr, &dsl);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    VkPipelineLayout layout;
    vkCreatePipelineLayout(d, &pli, nullptr, &layout);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const bool pipe_ok = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline) == VK_SUCCESS;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, u32(bindings.size())};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VkDescriptorPool pool;
    vkCreateDescriptorPool(d, &dpi, nullptr, &pool);
    bool ran = false;
    if (pipe_ok) {
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl;
        VkDescriptorSet set;
        vkAllocateDescriptorSets(d, &dai, &set);
        std::vector<VkDescriptorBufferInfo> bi(bindings.size());
        std::vector<VkWriteDescriptorSet> w(bindings.size());
        for (size_t i = 0; i < bindings.size(); ++i) {
            bi[i] = {bufs[i].buffer, 0, VK_WHOLE_SIZE};
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = set;
            w[i].dstBinding = bindings[i];
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(d, u32(w.size()), w.data(), 0, nullptr);
        ran = dev.Submit([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, 1, 1, 1);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        });
        if (ran) {
            const u32* o = static_cast<const u32*>(bufs.back().mapped);
            out.assign(o, o + 256);
        }
    }
    vkDestroyDescriptorPool(d, pool, nullptr);
    if (pipeline) vkDestroyPipeline(d, pipeline, nullptr);
    vkDestroyPipelineLayout(d, layout, nullptr);
    vkDestroyDescriptorSetLayout(d, dsl, nullptr);
    vkDestroyShaderModule(d, module, nullptr);
    for (auto& buf : bufs) dev.DestroyBuffer(buf);
    return ran;
}
} // namespace

TEST(Spirv_FuzzOnVulkan) {
    VK::Device* dev = SpirvDevice();
    if (!dev) { std::printf("    (Vulkan no disponible: solo se prueba la traduccion)\n"); return; }
    auto cases = LoadFuzz();
    const char* limit_env = std::getenv("NEXO2_SPIRV_FUZZ_COUNT");
    const size_t limit = limit_env ? size_t(std::atoi(limit_env)) : cases.size();
    u32 programs = 0, programs_ok = 0, shown = 0;
    for (size_t n = 0; n < cases.size() && n < limit; ++n) {
        auto& c = cases[n];
        if (!c.parsed) continue;
        ShaderProgram prog = MakeProgram(c.dksh);
        SpirvOptions opt;
        opt.int64 = true;
        const SpirvResult r = TranslateToSpirv(prog, opt);
        ++programs;
        if (!r.ok) continue;
        std::vector<u32> out;
        std::map<u32, std::vector<u32>> cbufs{{1, c.dksh.cb1}, {2, c.ubo}};
        if (!RunCompute(*dev, r, cbufs, out)) {
            if (shown++ < 3) std::printf("    prueba %zu: Vulkan no pudo ejecutarla\n", n);
            continue;
        }
        bool bad = false;
        for (u32 k = 0; k < 16; ++k) {
            const u32 got = out[(0x80 + 4 * k) / 4];
            if (!OutputOk(c.outs[k], got)) {
                if (!bad && shown++ < 5)
                    std::printf("    prueba %zu, salida %u: 0x%08X (%g), esperado 0x%08X (%g)\n%s\n", n, k, got, AsF(got),
                                c.outs[k].exp, AsF(c.outs[k].exp), c.glsl.c_str());
                bad = true;
            }
        }
        if (!bad) ++programs_ok;
    }
    std::printf("    %u programas en Vulkan (%s): %u bien\n", programs, dev->DeviceName().c_str(), programs_ok);
    CHECK_EQ(programs_ok, programs);
}
#endif

// Los shaders de pixeles de los tests de dibujo (compilados con uam) se traducen como etapa
// Fragment: entradas (IPA), salidas de color, discard y texturas.
#include "generated/shader_bins.hpp"
TEST(Spirv_TranslatesFragmentShaders) {
    struct Item { const char* name; const Tests::ShaderBin* bin; bool ok; };
    const Item items[] = {
        {"color", &Tests::k_color_frag, true},     {"discard", &Tests::k_discard_frag, true},
        {"tint", &Tests::k_tint_frag, true},       {"tex", &Tests::k_tex_frag, true},
        {"texarray", &Tests::k_texarray_frag, true}, {"texfetch", &Tests::k_texfetch_frag, true},
        {"texlod", &Tests::k_texlod_frag, true},   {"texshadow", &Tests::k_texshadow_frag, false},
    };
    const char* dump = std::getenv("NEXO2_SPIRV_DUMP");
    for (const Item& it : items) {
        ShaderProgram prog(it.bin->entry, [&](u64 addr, void* dst, size_t n) {
            std::memset(dst, 0, n);
            if (addr < it.bin->size) std::memcpy(dst, it.bin->code + addr, std::min<size_t>(n, it.bin->size - addr));
        });
        SpirvOptions opt;
        opt.stage = SpirvOptions::Stage::Fragment;
        const SpirvResult r = TranslateToSpirv(prog, opt);
        if (r.ok != it.ok) std::printf("  %s: %s\n", it.name, r.ok ? "traducido (se esperaba fallo)" : r.error.c_str());
        CHECK_EQ(r.ok, it.ok);
        if (!r.ok) continue;
        CHECK(r.code_end > 0x50);
        if (dump) {
            std::ofstream o(std::string(dump) + "/frag_" + it.name + ".spv", std::ios::binary);
            o.write(reinterpret_cast<const char*>(r.code.data()), std::streamsize(r.code.size() * 4));
        }
    }
    // Shader de vertices de paso, con los tres tipos de entrada
    const std::vector<u32> vs = BuildPassthroughVertexShader(
        {{8, SpirvVarying::Linear}, {9, SpirvVarying::Perspective}, {10, SpirvVarying::Flat}});
    CHECK(vs.size() > 20);
    if (dump) {
        std::ofstream o(std::string(dump) + "/passthrough.spv", std::ios::binary);
        o.write(reinterpret_cast<const char*>(vs.data()), std::streamsize(vs.size() * 4));
    }
}
