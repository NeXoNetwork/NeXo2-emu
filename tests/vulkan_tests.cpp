// Tests de Vulkan (GPU fase 3). Si el PC no tiene Vulkan (o ninguna GPU sirve), los tests
// lo dicen y pasan sin comprobar nada: NeXo sigue funcionando con el dibujo por software.
// En la nube se prueban con lavapipe (el Vulkan por software de Mesa).
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "test_framework.hpp"

#ifdef NEXO2_HAS_VULKAN
#include "video_core/vulkan/vk_device.hpp"
#include "generated/vk_test_spirv.hpp"

using namespace NeXo2;
namespace VK = NeXo2::GPU::Vulkan;

namespace {
// Un solo dispositivo para todos los tests (abrir Vulkan tarda)
VK::Device* SharedDevice() {
    static VK::Device dev;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (!dev.Init(std::getenv("NEXO2_VK_VALIDATION") != nullptr)) std::printf("    (Vulkan no disponible: %s; tests de Vulkan omitidos)\n", dev.Error().c_str());
        else std::printf("    (Vulkan: %s, %s)\n", dev.DeviceName().c_str(), dev.ApiVersionString().c_str());
    }
    return dev.Ok() ? &dev : nullptr;
}
} // namespace

TEST(Vulkan_DeviceInit) {
    VK::Device* dev = SharedDevice();
    if (!dev) return;
    CHECK(dev->Handle() != VK_NULL_HANDLE);
    CHECK(dev->Queue() != VK_NULL_HANDLE);
    CHECK(!dev->DeviceName().empty());
    // Un buffer visible desde la CPU: escribir y leer
    VK::Buffer b = dev->CreateBuffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    CHECK(b.buffer != VK_NULL_HANDLE && b.mapped);
    if (b.mapped) {
        // vkCmdFillBuffer desde la GPU y leerlo desde la CPU
        dev->Submit([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, b.buffer, 0, 4096, 0xC0FFEE11u); });
        CHECK_EQ(static_cast<const u32*>(b.mapped)[1023], 0xC0FFEE11u);
    }
    dev->DestroyBuffer(b);
}

TEST(Vulkan_ComputeSmoke) {
    // Un compute shader de verdad (SPIR-V de glslang): out[i] = in[i] * 2 + i
    VK::Device* dev = SharedDevice();
    if (!dev) return;
    VkDevice d = dev->Handle();
    constexpr u32 N = 256;
    VK::Buffer in = dev->CreateBuffer(N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    VK::Buffer out = dev->CreateBuffer(N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    CHECK(in.mapped && out.mapped);
    if (!in.mapped || !out.mapped) return;
    for (u32 i = 0; i < N; ++i) static_cast<u32*>(in.mapped)[i] = i * 7 + 3;

    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(Tests::k_spv_smoke_comp);
    smi.pCode = Tests::k_spv_smoke_comp;
    VkShaderModule module;
    CHECK_EQ(vkCreateShaderModule(d, &smi, nullptr, &module), VK_SUCCESS);

    VkDescriptorSetLayoutBinding binds[2]{};
    for (u32 i = 0; i < 2; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = binds;
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
    VkPipeline pipeline;
    CHECK_EQ(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline), VK_SUCCESS);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VkDescriptorPool pool;
    vkCreateDescriptorPool(d, &dpi, nullptr, &pool);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    vkAllocateDescriptorSets(d, &dai, &set);
    VkDescriptorBufferInfo bi[2] = {{in.buffer, 0, VK_WHOLE_SIZE}, {out.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (u32 i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(d, 2, w, 0, nullptr);

    CHECK(dev->Submit([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, N / 64, 1, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }));
    u32 bad = 0;
    for (u32 i = 0; i < N; ++i)
        if (static_cast<const u32*>(out.mapped)[i] != (i * 7 + 3) * 2 + i) ++bad;
    CHECK_EQ(bad, 0u);

    vkDestroyDescriptorPool(d, pool, nullptr);
    vkDestroyPipeline(d, pipeline, nullptr);
    vkDestroyPipelineLayout(d, layout, nullptr);
    vkDestroyDescriptorSetLayout(d, dsl, nullptr);
    vkDestroyShaderModule(d, module, nullptr);
    dev->DestroyBuffer(in);
    dev->DestroyBuffer(out);
}

#endif // NEXO2_HAS_VULKAN

// Los tests de dibujo y de texturas otra vez, ahora dibujando con Vulkan (GPU fase 3c):
// las mismas comprobaciones de pixeles tienen que cumplirse.
#include "gpu_draw_fixture.hpp"
TEST(Vulkan_DrawAndTextureTests) {
    {
        NeXo2::GPU::Vulkan::Device probe;
        if (!probe.Init() || !probe.HasDynamicRendering()) {
            std::printf("    (sin Vulkan con dynamic rendering: %s)\n", probe.Error().c_str());
            return;
        }
    }
    g_vulkanDraw = true;
    g_drawsTotal = g_drawsHw = g_drawsGpuVs = 0;
    for (const auto& t : NeXo2::Tests::Registry()) {
        const std::string name = t.name;
        if (name.rfind("Draw_", 0) != 0 && name.rfind("Texture_", 0) != 0) continue;
        if (name == "Draw_NroTriangle") continue;   // programa entero (su propia GPU)
        const int before = NeXo2::Tests::g_failures;
        t.fn();
        std::printf("    %s %s\n", NeXo2::Tests::g_failures == before ? "ok   " : "FALLO", t.name);
    }
    g_vulkanDraw = false;
    std::printf("    draws con Vulkan: %llu de %llu (shader de vertices en la GPU: %llu)\n", g_drawsHw, g_drawsTotal, g_drawsGpuVs);
    CHECK(g_drawsHw > 0);
    CHECK(g_drawsGpuVs > 0);
}

// Profundidad borrada en la GPU en un envio y usada en el siguiente: la memoria (vieja) no
// debe contar como "la CPU la cambio" (antes se volvia a subir y los objetos lejanos desaparecian)
TEST(Vulkan_DepthClearedThenNextSubmit) {
    {
        NeXo2::GPU::Vulkan::Device probe;
        if (!probe.Init() || !probe.HasDynamicRendering()) return;
    }
    g_vulkanDraw = true;
    DrawFixture f;
    g_vulkanDraw = false;
    V tri[3];
    for (int i = 0; i < 3; ++i) { tri[i] = kTri[i]; tri[i].z = 0.75f; tri[i].r = 0; tri[i].g = 1; tri[i].b = 0; }
    f.Write(VB, tri, sizeof(tri));
    // La memoria de la profundidad tiene ceros (como al empezar): si se usara, nada pasaria la prueba
    Push a;
    f.Setup(a, Tests::k_tri_vert, Tests::k_color_frag);
    a.Cmd(0x3F8, {Hi(ZT), Lo(ZT), 0x0A, 0, 0});
    a.Cmd(0x48A, {W, H});
    a.Cmd(0x54E, {1});
    a.Cmd(0x4B3, {1});
    a.Cmd(0x4BA, {1});
    a.Cmd(0x4C3, {0x201});
    f.Clear(a, 0, 0, 0, 1);
    a.Cmd(0x364, {F(1.0f)});
    a.Cmd(0x674, {1});
    Push b;
    f.DrawArrays(b, 4, 0, 3);
    f.Write(PB, a.w.data(), a.w.size() * 4);
    f.Write(PB + 0x2000, b.w.data(), b.w.size() * 4);
    f.ch.SubmitGpfifo({PB | (u64(a.w.size()) << 42)});
    f.ch.SubmitGpfifo({(PB + 0x2000) | (u64(b.w.size()) << 42)});
    f.gpu.FlushRenderer();
    CHECK_EQ(f.gpu.GetStats().draws_hw, 1u);
    CHECK(f.Pixel(32, 40)[1] == 255);
}
