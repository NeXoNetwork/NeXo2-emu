// Dibujo con Vulkan (ver vk_rasterizer.hpp y docs/07-nexo-internals/gpu-vulkan.md).
#include "vk_rasterizer.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <chrono>
#include <optional>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace NeXo2::GPU {
using Common::Logger;

namespace {

// --- Registros del motor 3D (los mismos que usa rasterizer.cpp) ---
constexpr u32 RT_BASE = 0x200;
constexpr u32 VIEWPORT_XFORM = 0x280;
constexpr u32 VIEWPORT = 0x300;
constexpr u32 SCISSOR = 0x380;
constexpr u32 ZT_ADDR = 0x3F8, ZT_FORMAT = 0x3FA, ZT_BLOCK = 0x3FB, ZT_LAYER_SIZE = 0x3FC;
constexpr u32 SCREEN_SCISSOR_H = 0x3FD, SCREEN_SCISSOR_V = 0x3FE;
constexpr u32 SINGLE_CT_WRITE = 0x3E4, MRT_ENABLE = 0x3EB;
constexpr u32 CLEAR_FLAGS = 0x43E;
constexpr u32 RT_CONTROL = 0x487;
constexpr u32 ZT_WIDTH = 0x48A, ZT_HEIGHT = 0x48B;
constexpr u32 DEPTH_TEST = 0x4B3, BLEND_PER_TARGET = 0x4B9, DEPTH_WRITE = 0x4BA, DEPTH_FUNC = 0x4C3;
constexpr u32 BLEND_CONST = 0x4C7;
constexpr u32 BLEND_GLOBAL = 0x4CF;
constexpr u32 BLEND_ENABLE = 0x4D8;
constexpr u32 WINDOW_ORIGIN = 0x4EB;
constexpr u32 ZT_SELECT = 0x54E;
constexpr u32 TSC_POOL = 0x557, TSC_MAX = 0x559;
constexpr u32 TIC_POOL = 0x55D, TIC_MAX = 0x55F;
constexpr u32 MULTISAMPLE_MODE = 0x574;
constexpr u32 PROGRAM_REGION = 0x582;
constexpr u32 CULL_ENABLE = 0x646, FRONT_FACE = 0x647, CULL_FACE = 0x648;
constexpr u32 VIEWPORT_XFORM_ENABLE = 0x64B;
constexpr u32 CT_WRITE = 0x680;
constexpr u32 BLEND_TARGET = 0x780;
constexpr u32 PIPELINE = 0x800;
constexpr u32 BINDLESS_TEXTURE = 0x982;
constexpr u32 CLEAR_COLOR = 0x360, CLEAR_DEPTH = 0x364;

constexpr u32 MAX_TEXTURES = 16;
constexpr u32 SETS_PER_POOL = 1024;
constexpr VkDeviceSize UPLOAD_SIZE = 64ull << 20;

// Suma el tiempo de un bloque a un contador
struct Timer {
    u64& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit Timer(u64& a) : acc(a) {}
    ~Timer() { acc += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
};

inline u64 Iova(u32 hi, u32 lo) { return (u64(hi & 0xFF) << 32) | lo; }
inline float F(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
inline u32 U(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }
inline VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) { return a ? (v + a - 1) / a * a : v; }

u64 Hash(const u8* d, size_t n) {
    u64 h = 0xCBF29CE484222325ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 v;
        std::memcpy(&v, d + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ d[i]) * 0x100000001B3ull;
    return h;
}

// Formato de color de Maxwell -> formato de Vulkan con los mismos bytes en memoria.
// kind: 0 float/unorm/snorm, 1 entero con signo, 2 entero sin signo.
bool ColorFormat(u32 f, VkFormat& vk, u8& kind) {
    kind = 0;
    switch (f) {
        case 0xC0: case 0xC3: vk = VK_FORMAT_R32G32B32A32_SFLOAT; return true;
        case 0xC1: case 0xC4: vk = VK_FORMAT_R32G32B32A32_SINT; kind = 1; return true;
        case 0xC2: case 0xC5: vk = VK_FORMAT_R32G32B32A32_UINT; kind = 2; return true;
        case 0xC6: vk = VK_FORMAT_R16G16B16A16_UNORM; return true;
        case 0xC7: vk = VK_FORMAT_R16G16B16A16_SNORM; return true;
        case 0xC8: vk = VK_FORMAT_R16G16B16A16_SINT; kind = 1; return true;
        case 0xC9: vk = VK_FORMAT_R16G16B16A16_UINT; kind = 2; return true;
        case 0xCA: case 0xCE: vk = VK_FORMAT_R16G16B16A16_SFLOAT; return true;
        case 0xCB: vk = VK_FORMAT_R32G32_SFLOAT; return true;
        case 0xCC: vk = VK_FORMAT_R32G32_SINT; kind = 1; return true;
        case 0xCD: vk = VK_FORMAT_R32G32_UINT; kind = 2; return true;
        case 0xD5: case 0xF9: vk = VK_FORMAT_R8G8B8A8_UNORM; return true;
        case 0xD6: case 0xFA: vk = VK_FORMAT_R8G8B8A8_SRGB; return true;
        case 0xD7: vk = VK_FORMAT_R8G8B8A8_SNORM; return true;
        case 0xD8: vk = VK_FORMAT_R8G8B8A8_SINT; kind = 1; return true;
        case 0xD9: vk = VK_FORMAT_R8G8B8A8_UINT; kind = 2; return true;
        case 0xCF: case 0xE6: case 0xFD: case 0xFE: vk = VK_FORMAT_B8G8R8A8_UNORM; return true;
        case 0xD0: case 0xE7: vk = VK_FORMAT_B8G8R8A8_SRGB; return true;
        case 0xD1: vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return true;
        case 0xDF: vk = VK_FORMAT_A2R10G10B10_UNORM_PACK32; return true;
        case 0xD2: vk = VK_FORMAT_A2B10G10R10_UINT_PACK32; kind = 2; return true;
        case 0xDA: vk = VK_FORMAT_R16G16_UNORM; return true;
        case 0xDB: vk = VK_FORMAT_R16G16_SNORM; return true;
        case 0xDC: vk = VK_FORMAT_R16G16_SINT; kind = 1; return true;
        case 0xDD: vk = VK_FORMAT_R16G16_UINT; kind = 2; return true;
        case 0xDE: vk = VK_FORMAT_R16G16_SFLOAT; return true;
        case 0xE0: vk = VK_FORMAT_B10G11R11_UFLOAT_PACK32; return true;
        case 0xE3: vk = VK_FORMAT_R32_SINT; kind = 1; return true;
        case 0xE4: case 0xFF: vk = VK_FORMAT_R32_UINT; kind = 2; return true;
        case 0xE5: vk = VK_FORMAT_R32_SFLOAT; return true;
        case 0xEA: vk = VK_FORMAT_R8G8_UNORM; return true;
        case 0xEB: vk = VK_FORMAT_R8G8_SNORM; return true;
        case 0xEC: vk = VK_FORMAT_R8G8_SINT; kind = 1; return true;
        case 0xED: vk = VK_FORMAT_R8G8_UINT; kind = 2; return true;
        case 0xEE: vk = VK_FORMAT_R16_UNORM; return true;
        case 0xEF: vk = VK_FORMAT_R16_SNORM; return true;
        case 0xF0: vk = VK_FORMAT_R16_SINT; kind = 1; return true;
        case 0xF1: vk = VK_FORMAT_R16_UINT; kind = 2; return true;
        case 0xF2: vk = VK_FORMAT_R16_SFLOAT; return true;
        case 0xF3: vk = VK_FORMAT_R8_UNORM; return true;
        case 0xF4: vk = VK_FORMAT_R8_SNORM; return true;
        case 0xF5: vk = VK_FORMAT_R8_SINT; kind = 1; return true;
        case 0xF6: vk = VK_FORMAT_R8_UINT; kind = 2; return true;
        default: return false;
    }
}

// Mezcla: valores de OpenGL y de Direct3D, como BlendFactor/BlendEquation del software
VkBlendFactor BlendFactorVk(u32 f) {
    switch (f) {
        case 0x4000: case 0x01: return VK_BLEND_FACTOR_ZERO;
        case 0x4001: case 0x02: return VK_BLEND_FACTOR_ONE;
        case 0x4300: case 0x03: case 0xC900: case 0x10: return VK_BLEND_FACTOR_SRC_COLOR;
        case 0x4301: case 0x04: case 0xC901: case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x4302: case 0x05: case 0xC902: case 0x12: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x4303: case 0x06: case 0xC903: case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x4304: case 0x07: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x4305: case 0x08: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x4306: case 0x09: return VK_BLEND_FACTOR_DST_COLOR;
        case 0x4307: case 0x0A: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x4308: case 0x0B: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0xC001: case 0x0E: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0xC002: case 0x0F: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0xC003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0xC004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp BlendOpVk(u32 op) {
    switch (op) {
        case 0x800A: case 0x02: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: case 0x03: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: case 0x04: return VK_BLEND_OP_MIN;
        case 0x8008: case 0x05: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}
VkCompareOp CompareOpVk(u32 func) {
    const u32 f = func >= 0x200 ? func - 0x200 : func - 1;
    return VkCompareOp(f & 7);   // mismo orden: nunca, <, ==, <=, >, !=, >=, siempre
}
VkSamplerAddressMode WrapVk(u32 w) {
    switch (w) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 2: case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;   // "espejo una vez": lo mas parecido
    }
}
VkComponentSwizzle SwizzleVk(u32 s) {
    switch (s) {
        case 2: return VK_COMPONENT_SWIZZLE_R;
        case 3: return VK_COMPONENT_SWIZZLE_G;
        case 4: return VK_COMPONENT_SWIZZLE_B;
        case 5: return VK_COMPONENT_SWIZZLE_A;
        case 6: case 7: return VK_COMPONENT_SWIZZLE_ONE;
        default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}

// Posicion dentro de un GOB del byte 'o' (0..63) de una fila
inline u32 GobColumn(u32 o) { return (o / 32) * 256 + ((o % 32) / 16) * 32 + (o % 16); }

// Memoria de la superficie (SizeBytes, block linear o lineal) <-> filas seguidas
// (width * bpp bytes por fila). 'ToGuest' = de las filas a la memoria.
template <bool ToGuest>
void Swizzle(const Surface& s, u8* guest, u8* rows) {
    const u32 bpp = s.bytes_per_pixel, row_bytes = s.width * bpp;
    for (u32 y = 0; y < s.height; ++y) {
        u8* row = rows + size_t(y) * row_bytes;
        if (s.linear) {
            u8* g = guest + size_t(y) * s.pitch;
            if constexpr (ToGuest) std::memcpy(g, row, row_bytes); else std::memcpy(row, g, row_bytes);
            continue;
        }
        for (u32 xb = 0; xb < row_bytes; xb += 64) {
            u8* g = guest + BlockLinearOffset(xb, y, row_bytes, s.block_height_log2);
            const u32 n = std::min(64u, row_bytes - xb);
            for (u32 o = 0; o < n; o += 16) {
                const u32 piece = std::min(16u, n - o);
                if constexpr (ToGuest) std::memcpy(g + GobColumn(o), row + xb + o, piece);
                else std::memcpy(row + xb + o, g + GobColumn(o), piece);
            }
        }
    }
}

// float -> half (para texturas si la GPU no filtra floats de 32 bits)
u16 ToHalf(float f) {
    u32 x = U(f);
    const u32 sign = (x >> 16) & 0x8000;
    s32 e = s32((x >> 23) & 0xFF) - 127 + 15;
    u32 m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return u16(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return u16(sign | 0x7C00);
    if (e <= 0) {
        if (e < -10) return u16(sign);
        m |= 0x800000;
        const u32 shift = u32(14 - e);
        return u16(sign | ((m + (1u << (shift - 1))) >> shift));
    }
    const u32 r = sign | (u32(e) << 10) | (m >> 13);
    return u16(r + ((m >> 12) & 1));
}

// Formato de Vulkan de una textura sin comprimir (para leer un render target como textura)
VkFormat TicVkFormat(const TextureInfo& t) {
    const u32 type = t.types[0];   // 1 snorm, 2 unorm, 7 float
    switch (t.format) {
        case 0x01: return type == 7 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_UNDEFINED;
        case 0x03: return type == 7 ? VK_FORMAT_R16G16B16A16_SFLOAT : type == 2 ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_UNDEFINED;
        case 0x07: case 0x08:
            if (type != 2) return VK_FORMAT_UNDEFINED;
            return t.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        case 0x09: return type == 2 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_UNDEFINED;
        case 0x0C: return type == 7 ? VK_FORMAT_R16G16_SFLOAT : type == 2 ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_UNDEFINED;
        case 0x0F: return type == 7 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_UNDEFINED;
        case 0x18: return type == 2 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_UNDEFINED;
        case 0x1B: return type == 7 ? VK_FORMAT_R16_SFLOAT : type == 2 ? VK_FORMAT_R16_UNORM : VK_FORMAT_UNDEFINED;
        case 0x1D: return type == 2 ? VK_FORMAT_R8_UNORM : VK_FORMAT_UNDEFINED;
        case 0x21: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}

} // namespace

// Estado fijo de un pipeline (lo que no es dinamico)
struct PipelineDesc {
    std::vector<SpirvVarying> varyings;
    u32 ncolor = 0;
    VkFormat color[8] = {};
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkPipelineColorBlendAttachmentState blend[8] = {};
    bool depth_test = false, depth_write = false;
    VkCompareOp depth_op = VK_COMPARE_OP_ALWAYS;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
};

// ============================================================================
//  Arranque
// ============================================================================

VulkanRasterizer::VulkanRasterizer(Gpu& gpu, Vulkan::Device& device) : m_gpu(gpu), m_dev(device), m_vk(device.Handle()) {}

VulkanRasterizer::~VulkanRasterizer() {
    if (!m_vk) return;
    vkDeviceWaitIdle(m_vk);
    for (auto& img : m_images) DestroyImage(*img);
    for (auto& [k, t] : m_textures) {
        if (t->view) vkDestroyImageView(m_vk, t->view, nullptr);
        if (t->image) vkDestroyImage(m_vk, t->image, nullptr);
        if (t->memory) vkFreeMemory(m_vk, t->memory, nullptr);
    }
    for (auto& [k, s] : m_samplers) vkDestroySampler(m_vk, s, nullptr);
    for (auto& [a, list] : m_fragShaders)
        for (auto& fs : list) if (fs->module) vkDestroyShaderModule(m_vk, fs->module, nullptr);
    for (auto& [k, m] : m_vertShaders) vkDestroyShaderModule(m_vk, m, nullptr);
    for (auto& [k, p] : m_pipelines) vkDestroyPipeline(m_vk, p, nullptr);
    m_dev.DestroyBuffer(m_upload);
    m_dev.DestroyBuffer(m_readback);
    if (m_descPool) vkDestroyDescriptorPool(m_vk, m_descPool, nullptr);
    if (m_pipeLayout) vkDestroyPipelineLayout(m_vk, m_pipeLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_vk, m_setLayout, nullptr);
    if (m_fence) vkDestroyFence(m_vk, m_fence, nullptr);
    if (m_pool) vkDestroyCommandPool(m_vk, m_pool, nullptr);
}

bool VulkanRasterizer::Init(std::string& error) {
    if (!m_dev.Ok()) { error = "Vulkan no esta abierto"; return false; }
    if (!m_dev.HasDynamicRendering()) { error = "la GPU no tiene dynamic rendering (Vulkan 1.3)"; return false; }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_dev.QueueFamily();
    if (vkCreateCommandPool(m_vk, &pci, nullptr, &m_pool) != VK_SUCCESS) { error = "vkCreateCommandPool"; return false; }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_vk, &ai, &m_cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m_vk, &fci, nullptr, &m_fence);

    // Enlaces: 0..17 constbufs, 18 informacion del draw, 32.. texturas (todo en el shader de pixeles)
    std::vector<VkDescriptorSetLayoutBinding> binds;
    for (u32 i = 0; i <= SpirvBinding::DRAW_INFO; ++i)
        binds.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (u32 i = 0; i < MAX_TEXTURES; ++i)
        binds.push_back({SpirvBinding::TEXTURE0 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = u32(binds.size());
    dl.pBindings = binds.data();
    if (vkCreateDescriptorSetLayout(m_vk, &dl, nullptr, &m_setLayout) != VK_SUCCESS) { error = "vkCreateDescriptorSetLayout"; return false; }
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_setLayout;
    if (vkCreatePipelineLayout(m_vk, &pl, nullptr, &m_pipeLayout) != VK_SUCCESS) { error = "vkCreatePipelineLayout"; return false; }
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (SpirvBinding::DRAW_INFO + 1) * SETS_PER_POOL},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES * SETS_PER_POOL},
    };
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = SETS_PER_POOL;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_vk, &dp, nullptr, &m_descPool) != VK_SUCCESS) { error = "vkCreateDescriptorPool"; return false; }
    m_setsLeft = SETS_PER_POOL;

    if (!EnsureBuffer(m_upload, UPLOAD_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        error = "sin memoria para el buffer de subida";
        return false;
    }

    // Texturas: RGBA de 32 bits float si se pueden filtrar; si no, de 16 bits
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(m_dev.Physical(), VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) m_texFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    Logger::Log(Logger::Level::Info, std::string("[Vulkan] Dibujo con la GPU listo (texturas en ") +
                                         (m_texFormat == VK_FORMAT_R32G32B32A32_SFLOAT ? "RGBA32F" : "RGBA16F") + ")");
    return true;
}

bool VulkanRasterizer::EnsureBuffer(Vulkan::Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (b.buffer && b.size >= size) return true;
    m_dev.DestroyBuffer(b);
    b = m_dev.CreateBuffer(size, usage, true);
    return b.buffer != VK_NULL_HANDLE && b.mapped;
}

bool VulkanRasterizer::Fallback(const std::string& why) {
    ++m_stats.fallbacks;
    m_lastFallback = why;
    m_gpu.Warn("vkfb:" + why, "Vulkan: " + why + " (ese dibujo se hace por software)");
    return false;
}

// ============================================================================
//  Comandos
// ============================================================================

VkCommandBuffer VulkanRasterizer::Cmd() {
    if (!m_recording) {
        vkResetCommandBuffer(m_cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &bi);
        m_recording = true;
        m_lastOp = 0;
    }
    return m_cmd;
}

void VulkanRasterizer::EndRendering() {
    if (!m_inRendering) return;
    m_dev.CmdEndRendering(m_cmd);
    m_inRendering = false;
    m_renderColor.clear();
    m_renderDepth = nullptr;
}

void VulkanRasterizer::Submit() {
    if (!m_recording) return;
    EndRendering();
    vkEndCommandBuffer(m_cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmd;
    vkResetFences(m_vk, 1, &m_fence);
    VkResult r = vkQueueSubmit(m_dev.Queue(), 1, &si, m_fence);
    if (r == VK_SUCCESS) { Timer t(m_stats.ns_wait); r = vkWaitForFences(m_vk, 1, &m_fence, VK_TRUE, 20'000'000'000ull); }
    if (r != VK_SUCCESS) m_gpu.Warn("vksubmit", std::string("Vulkan: fallo al ejecutar (") + Vulkan::ResultName(r) + ")");
    m_recording = false;
    vkResetDescriptorPool(m_vk, m_descPool, 0);
    m_setsLeft = SETS_PER_POOL;
    m_uploadOffset = 0;
    m_lastOp = 0;
    ++m_stats.submits;
}

// Barrera "todo antes de todo": sencilla y suficiente (la GPU espera a que acabe lo anterior)
static void FullBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}
void VulkanRasterizer::TransferBarrier() {
    EndRendering();
    FullBarrier(Cmd());
    m_lastOp = 1;
}
void VulkanRasterizer::RenderBarrier() {
    if (m_lastOp != 2) FullBarrier(Cmd());
    m_lastOp = 2;
}

VulkanRasterizer::Alloc VulkanRasterizer::Allocate(VkDeviceSize size, VkDeviceSize align) {
    VkDeviceSize off = AlignUp(m_uploadOffset, std::max<VkDeviceSize>(align, 16));
    if (off + size > m_upload.size) {
        Submit();   // lo grabado usa el buffer: ejecutarlo antes de reutilizarlo
        off = 0;
        if (size > m_upload.size)
            EnsureBuffer(m_upload, AlignUp(size, 1 << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    m_uploadOffset = off + size;
    return {off, static_cast<u8*>(m_upload.mapped) + off};
}

// ============================================================================
//  Imagenes (render targets y profundidad) y su copia en memoria
// ============================================================================

bool VulkanRasterizer::CreateImage(Image& img) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = img.format;
    ici.extent = {img.surf.width, img.surf.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (img.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_vk, &ici, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_vk, img.image, &req);
    s32 type = m_dev.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = m_dev.FindMemoryType(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = u32(std::max(type, 0));
    if (type < 0 || vkAllocateMemory(m_vk, &mai, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_vk, img.image, img.memory, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = img.format;
    vci.subresourceRange = {VkImageAspectFlags(img.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    if (vkCreateImageView(m_vk, &vci, nullptr, &img.view) != VK_SUCCESS) return false;
    // Todas las imagenes viven en el layout GENERAL (sirve para copiar y para dibujar)
    EndRendering();
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img.image;
    ib.subresourceRange = vci.subresourceRange;
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &ib);
    return true;
}

void VulkanRasterizer::DestroyImage(Image& img) {
    for (auto& [k, v] : img.sampled) vkDestroyImageView(m_vk, v, nullptr);
    img.sampled.clear();
    if (img.view) vkDestroyImageView(m_vk, img.view, nullptr);
    if (img.image) vkDestroyImage(m_vk, img.image, nullptr);
    if (img.memory) vkFreeMemory(m_vk, img.memory, nullptr);
    img.view = VK_NULL_HANDLE;
    img.image = VK_NULL_HANDLE;
    img.memory = VK_NULL_HANDLE;
}

VulkanRasterizer::Image* VulkanRasterizer::GetImage(const Surface& surf, u32 guest_format, bool depth, bool will_overwrite) {
    const u64 lo = surf.address, hi = surf.address + surf.SizeBytes();
    Image* found = nullptr;
    std::vector<Image*> evict;
    for (auto& up : m_images) {
        Image& im = *up;
        const bool same = im.surf.address == surf.address && im.guest_format == guest_format && im.depth == depth &&
                          im.surf.width == surf.width && im.surf.height == surf.height && im.surf.linear == surf.linear &&
                          im.surf.pitch == surf.pitch && im.surf.block_height_log2 == surf.block_height_log2 &&
                          im.surf.bytes_per_pixel == surf.bytes_per_pixel;
        if (same) found = &im;
        else if (lo < im.hi && hi > im.lo) evict.push_back(&im);
    }
    if (!evict.empty()) {
        // Otra imagen en la misma memoria (otro formato o tamano): a memoria y fuera
        FlushImages(evict);
        Submit();
        for (Image* e : evict) {
            DestroyImage(*e);
            m_images.erase(std::remove_if(m_images.begin(), m_images.end(), [e](auto& p) { return p.get() == e; }), m_images.end());
        }
    }
    if (!found) {
        auto img = std::make_unique<Image>();
        img->surf = surf;
        img->guest_format = guest_format;
        img->depth = depth;
        img->lo = lo;
        img->hi = hi;
        if (depth) img->format = VK_FORMAT_D32_SFLOAT;
        else {
            u8 kind;
            ColorFormat(guest_format, img->format, kind);
        }
        if (!CreateImage(*img)) { DestroyImage(*img); return nullptr; }
        found = img.get();
        m_images.push_back(std::move(img));
    }
    if (will_overwrite) {
        // Se va a borrar entera: no hace falta lo que haya en memoria
        found->mem_dirty = false;
        found->epoch = m_epoch;
        return found;
    }
    if (found->epoch != m_epoch && !found->mem_dirty) {
        // Nuevo envio: la CPU puede haber escrito en la memoria. Se compara su hash.
        std::vector<u8> guest(size_t(hi - lo));
        m_busy = true;
        m_gpu.MemoryManager().ReadBlock(lo, guest.data(), guest.size());
        m_busy = false;
        if (Hash(guest.data(), guest.size()) != found->hash) {
            if (found->gpu_dirty) {
                m_gpu.Warn("vkconflict", "Vulkan: la CPU cambio un render target que la GPU tambien cambio (gana la CPU)");
                found->gpu_dirty = false;
                --m_dirtyCount;
            }
            found->mem_dirty = true;
        }
    }
    found->epoch = m_epoch;
    if (found->mem_dirty) Upload(*found);
    return found;
}

void VulkanRasterizer::Upload(Image& img) {
    const Surface& s = img.surf;
    std::vector<u8> guest(size_t(s.SizeBytes()));
    m_busy = true;
    m_gpu.MemoryManager().ReadBlock(s.address, guest.data(), guest.size());
    m_busy = false;
    img.hash = Hash(guest.data(), guest.size());
    const size_t pixels = size_t(s.width) * s.height;
    std::vector<u8> rows(pixels * s.bytes_per_pixel);
    Swizzle<false>(s, guest.data(), rows.data());
    const size_t stage_size = img.depth ? pixels * 4 : rows.size();
    const Alloc a = Allocate(stage_size, 16);
    if (img.depth) {
        float* out = reinterpret_cast<float*>(a.ptr);
        for (size_t i = 0; i < pixels; ++i) {
            float d = 0;
            DecodeDepth(img.guest_format, &rows[i * s.bytes_per_pixel], d);
            out[i] = d;
        }
    } else {
        std::memcpy(a.ptr, rows.data(), rows.size());
    }
    TransferBarrier();
    VkBufferImageCopy r{};
    r.bufferOffset = a.offset;
    r.imageSubresource = {VkImageAspectFlags(img.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    r.imageExtent = {s.width, s.height, 1};
    vkCmdCopyBufferToImage(Cmd(), m_upload.buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    img.mem_dirty = false;
    ++m_stats.uploads;
}

void VulkanRasterizer::Download(const std::vector<Image*>& imgs) {
    if (imgs.empty()) return;
    VkDeviceSize total = 0;
    std::vector<VkDeviceSize> offs;
    for (Image* im : imgs) {
        offs.push_back(total);
        total += AlignUp(VkDeviceSize(im->surf.width) * im->surf.height * (im->depth ? 4 : im->surf.bytes_per_pixel), 16);
    }
    if (!m_readback.buffer || m_readback.size < total) {
        Submit();
        EnsureBuffer(m_readback, AlignUp(total, 1 << 20), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    TransferBarrier();
    for (size_t i = 0; i < imgs.size(); ++i) {
        const Image& im = *imgs[i];
        VkBufferImageCopy r{};
        r.bufferOffset = offs[i];
        r.imageSubresource = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
        r.imageExtent = {im.surf.width, im.surf.height, 1};
        vkCmdCopyImageToBuffer(Cmd(), im.image, VK_IMAGE_LAYOUT_GENERAL, m_readback.buffer, 1, &r);
    }
    FullBarrier(Cmd());   // que la CPU vea lo copiado
    Submit();
    Timer tdl(m_stats.ns_download);
    auto& mm = m_gpu.MemoryManager();
    for (size_t i = 0; i < imgs.size(); ++i) {
        Image& im = *imgs[i];
        const Surface& s = im.surf;
        const size_t pixels = size_t(s.width) * s.height;
        std::vector<u8> guest(size_t(s.SizeBytes()));
        std::vector<u8> rows(pixels * s.bytes_per_pixel);
        const u8* src = static_cast<const u8*>(m_readback.mapped) + offs[i];
        m_busy = true;
        mm.ReadBlock(s.address, guest.data(), guest.size());   // relleno y stencil, tal cual
        if (im.depth) {
            Swizzle<false>(s, guest.data(), rows.data());
            for (size_t p = 0; p < pixels; ++p) {
                float d;
                std::memcpy(&d, src + p * 4, 4);
                EncodeDepthStencil(im.guest_format, d, 0, true, false, &rows[p * s.bytes_per_pixel]);
            }
        } else {
            std::memcpy(rows.data(), src, rows.size());
        }
        Swizzle<true>(s, guest.data(), rows.data());
        mm.WriteBlock(s.address, guest.data(), guest.size());   // avisa a la cache de texturas
        m_busy = false;
        im.hash = Hash(guest.data(), guest.size());
        im.epoch = m_epoch;
        if (im.gpu_dirty) { im.gpu_dirty = false; --m_dirtyCount; }
        ++m_stats.downloads;
    }
}

void VulkanRasterizer::FlushImages(const std::vector<Image*>& imgs) {
    std::vector<Image*> dirty;
    for (Image* im : imgs) if (im->gpu_dirty) dirty.push_back(im);
    if (dirty.empty()) return;
    const bool was_busy = m_busy;
    m_busy = true;
    Download(dirty);
    m_busy = was_busy;
}

void VulkanRasterizer::Flush(bool depth) {
    std::vector<Image*> all;
    for (auto& im : m_images) if (depth || (!im->depth && im->rendered)) all.push_back(im.get());
    FlushImages(all);
    Submit();
}

void VulkanRasterizer::FlushRegion(u64 va, u64 size) {
    if (m_dirtyCount == 0) return;
    std::vector<Image*> hit;
    for (auto& im : m_images)
        if (im->gpu_dirty && va < im->hi && va + size > im->lo) hit.push_back(im.get());
    FlushImages(hit);
}

void VulkanRasterizer::BeforeAccess(u64 va, u64 size, bool) {
    if (m_busy || m_dirtyCount == 0) return;
    FlushRegion(va, size);
}

void VulkanRasterizer::OnGpuWrite(u64 va, u64 size) {
    if (m_busy) return;   // lo estamos escribiendo nosotros
    for (auto& im : m_images)
        if (va < im->hi && va + size > im->lo) {
            if (im->gpu_dirty) { im->gpu_dirty = false; --m_dirtyCount; }   // (BeforeAccess ya lo escribio)
            im->mem_dirty = true;
        }
}

void VulkanRasterizer::Reset() {
    if (m_recording) Submit();
    vkDeviceWaitIdle(m_vk);
    for (auto& im : m_images) DestroyImage(*im);
    m_images.clear();
    m_dirtyCount = 0;
    for (auto& [k, t] : m_textures) {
        if (t->view) vkDestroyImageView(m_vk, t->view, nullptr);
        if (t->image) vkDestroyImage(m_vk, t->image, nullptr);
        if (t->memory) vkFreeMemory(m_vk, t->memory, nullptr);
    }
    m_textures.clear();
}

// ============================================================================
//  Texturas
// ============================================================================

VkSampler VulkanRasterizer::GetSampler(const SamplerInfo& s, float tic_bias, bool no_mips) {
    const float bias = std::clamp(s.lod_bias + tic_bias, -m_dev.Limits().maxSamplerLodBias, m_dev.Limits().maxSamplerLodBias);
    // Color de borde: Vulkan (sin extensiones) solo tiene negro transparente, negro y blanco
    VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (s.border[0] >= 0.5f && s.border[1] >= 0.5f && s.border[2] >= 0.5f) border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (s.border[3] >= 0.5f) border = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    const std::vector<u32> key{s.wrap[0], s.wrap[1], s.wrap[2], s.mag_filter, s.min_filter, s.mip_filter, U(bias),
                               U(s.min_lod), U(s.max_lod), u32(no_mips), u32(border)};
    auto it = m_samplers.find(key);
    if (it != m_samplers.end()) return it->second;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = s.mag_filter >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = s.min_filter >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = s.mip_filter == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = WrapVk(s.wrap[0]);
    sci.addressModeV = WrapVk(s.wrap[1]);
    sci.addressModeW = WrapVk(s.wrap[2]);
    sci.mipLodBias = bias;
    sci.minLod = no_mips ? 0.0f : s.min_lod;
    sci.maxLod = no_mips ? 0.25f : std::max(s.min_lod, s.max_lod);   // "sin mipmaps": solo el nivel base
    sci.borderColor = border;
    VkSampler smp = VK_NULL_HANDLE;
    vkCreateSampler(m_vk, &sci, nullptr, &smp);
    m_samplers[key] = smp;
    return smp;
}

VulkanRasterizer::Image* VulkanRasterizer::RenderTargetAsTexture(const TextureInfo& info, const SpirvTexture& want) {
    if (want.dim != 1 || want.array || !(info.type == 1 || info.type == 5 || info.type == 7)) return nullptr;
    if (info.min_level != 0) return nullptr;
    const VkFormat fmt = TicVkFormat(info);
    if (fmt == VK_FORMAT_UNDEFINED) return nullptr;
    for (auto& up : m_images) {
        Image& im = *up;
        if (im.depth || im.surf.address != info.address || im.format != fmt) continue;
        if (im.surf.width != info.width || im.surf.height != info.height) continue;
        if (im.surf.linear == info.block_linear) continue;
        if (info.block_linear && im.surf.block_height_log2 != info.block_height_log2) continue;
        if (!info.block_linear && im.surf.pitch != info.pitch) continue;
        // Al dia con la memoria (por si la CPU la cambio) y lista para leer
        Image* cur = GetImage(im.surf, im.guest_format, false, false);
        return cur;
    }
    return nullptr;
}

bool VulkanRasterizer::PrepareTexture(u32 handle, const SpirvTexture& want, u64 tic_pool, u32 tic_max, u64 tsc_pool,
                                      u32 tsc_max, BoundTexture& out) {
    auto& mm = m_gpu.MemoryManager();
    const u32 tic_index = handle & 0xFFFFF, tsc_index = handle >> 20;
    if (tic_index > tic_max || tsc_index > tsc_max) return Fallback("textura fuera de la tabla");
    std::array<u32, 8> tic{}, tsc{};
    mm.ReadBlock(tic_pool + u64(tic_index) * 32, tic.data(), 32);
    mm.ReadBlock(tsc_pool + u64(tsc_index) * 32, tsc.data(), 32);
    const TextureInfo info = DecodeTic(tic.data());
    const SamplerInfo smp = DecodeTsc(tsc.data());
    if (!info.valid) return Fallback("textura con formato desconocido");
    for (u32 c = 0; c < 4; ++c)
        if (info.types[c] == 3 || info.types[c] == 4) return Fallback("textura de formato entero");
    if (smp.depth_compare) return Fallback("sampler con comparacion de profundidad");
    // La forma que pide el shader tiene que caber en la textura
    const u32 t = info.type;
    bool ok = false;
    switch (want.dim) {
        case 0: ok = t == 0 || t == 4; break;
        case 1: ok = t == 1 || t == 5 || t == 7; break;
        case 2: ok = t == 2; break;
        case 3: ok = t == 3; break;
    }
    if (!ok) return Fallback("la textura no tiene la forma que pide el shader");

    // Un render target que esta en la GPU: se lee la imagen directamente (sin escribirla en
    // memoria, decodificarla y volver a subirla, que es lo que mas costaba)
    if (Image* rt = RenderTargetAsTexture(info, want)) {
        const u32 sw = info.swizzle[0] | (info.swizzle[1] << 3) | (info.swizzle[2] << 6) | (info.swizzle[3] << 9);
        VkImageView& view = rt->sampled[sw];
        if (!view) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = rt->image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = rt->format;
            vci.components = {SwizzleVk(info.swizzle[0]), SwizzleVk(info.swizzle[1]), SwizzleVk(info.swizzle[2]),
                              SwizzleVk(info.swizzle[3])};
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(m_vk, &vci, nullptr, &view);
        }
        out.view = view;
        out.sampler = GetSampler(smp, info.lod_bias, true);
        const bool norm = info.normalized && !smp.force_unnormalized;
        out.scale[0] = norm ? 1.0f : 1.0f / float(info.width);
        out.scale[1] = norm ? 1.0f : 1.0f / float(info.height);
        ++m_stats.rt_textures;
        return true;
    }

    // Niveles decodificados (cache de la CPU: solo decodifica si la memoria ha cambiado)
    const u32 nlevels = std::min<u32>(info.max_level - info.min_level + 1, 16);
    std::vector<std::shared_ptr<const DecodedLevel>> levels;
    for (u32 i = 0; i < nlevels; ++i) levels.push_back(m_gpu.Textures().Get(tic, info, info.min_level + i));
    const DecodedLevel& base = *levels[0];
    const bool is3d = want.dim == 2;
    const u32 layers = is3d ? 1 : base.depth;
    if (want.dim == 3 && layers % 6 != 0) return Fallback("cubo sin 6 caras");

    std::vector<u32> key(tic.begin(), tic.end());
    key.push_back(want.dim);
    key.push_back(want.array);
    auto& slot = m_textures[key];
    if (slot && (slot->width != base.width || slot->height != base.height || slot->depth != base.depth ||
                 slot->levels != nlevels)) {
        Submit();   // puede estar en uso
        vkDestroyImageView(m_vk, slot->view, nullptr);
        vkDestroyImage(m_vk, slot->image, nullptr);
        vkFreeMemory(m_vk, slot->memory, nullptr);
        slot.reset();
    }
    if (!slot) {
        auto tex = std::make_unique<Texture>();
        tex->tic = tic;
        tex->dim = want.dim;
        tex->array = want.array;
        tex->width = base.width;
        tex->height = base.height;
        tex->depth = base.depth;
        tex->levels = nlevels;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = want.dim == 0 ? VK_IMAGE_TYPE_1D : is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ici.format = m_texFormat;
        ici.extent = {base.width, want.dim == 0 ? 1 : base.height, is3d ? base.depth : 1};
        ici.mipLevels = nlevels;
        ici.arrayLayers = layers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (want.dim == 3) ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if (vkCreateImage(m_vk, &ici, nullptr, &tex->image) != VK_SUCCESS) return Fallback("no se pudo crear la textura");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_vk, tex->image, &req);
        s32 type = m_dev.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = m_dev.FindMemoryType(req.memoryTypeBits, 0);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = u32(std::max(type, 0));
        if (type < 0 || vkAllocateMemory(m_vk, &mai, nullptr, &tex->memory) != VK_SUCCESS) {
            vkDestroyImage(m_vk, tex->image, nullptr);
            return Fallback("sin memoria para la textura");
        }
        vkBindImageMemory(m_vk, tex->image, tex->memory, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = tex->image;
        switch (want.dim) {
            case 0: vci.viewType = want.array ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D; break;
            case 2: vci.viewType = VK_IMAGE_VIEW_TYPE_3D; break;
            case 3: vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE; break;
            default: vci.viewType = want.array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; break;
        }
        vci.format = m_texFormat;
        vci.components = {SwizzleVk(info.swizzle[0]), SwizzleVk(info.swizzle[1]), SwizzleVk(info.swizzle[2]),
                          SwizzleVk(info.swizzle[3])};
        const u32 view_layers = want.dim == 3 ? 6 : want.array ? layers : 1;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, nlevels, 0, view_layers};
        vkCreateImageView(m_vk, &vci, nullptr, &tex->view);
        EndRendering();
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = tex->image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, nlevels, 0, layers};
        vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &ib);
        tex->uploaded.assign(nlevels, nullptr);
        slot = std::move(tex);
        ++m_stats.textures;
    }
    // Subir los niveles que han cambiado
    Texture& tex = *slot;
    for (u32 i = 0; i < nlevels; ++i) {
        if (tex.uploaded[i] == levels[i]) continue;
        const DecodedLevel& lv = *levels[i];
        const size_t texels = lv.texels.size();
        const bool half = m_texFormat == VK_FORMAT_R16G16B16A16_SFLOAT;
        const Alloc a = Allocate(texels * (half ? 8 : 16), 16);
        if (half) {
            u16* o = reinterpret_cast<u16*>(a.ptr);
            for (size_t k = 0; k < texels; ++k)
                for (u32 c = 0; c < 4; ++c) o[k * 4 + c] = ToHalf(F(lv.texels[k][c]));
        } else {
            std::memcpy(a.ptr, lv.texels.data(), texels * 16);
        }
        TransferBarrier();
        VkBufferImageCopy r{};
        r.bufferOffset = a.offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, layers};
        r.imageExtent = {lv.width, want.dim == 0 ? 1 : lv.height, is3d ? lv.depth : 1};
        vkCmdCopyBufferToImage(Cmd(), m_upload.buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        tex.uploaded[i] = levels[i];
    }
    out.view = tex.view;
    out.sampler = GetSampler(smp, info.lod_bias, smp.mip_filter <= 1);
    const bool norm = info.normalized && !smp.force_unnormalized;
    out.scale[0] = norm ? 1.0f : 1.0f / float(base.width);
    out.scale[1] = norm ? 1.0f : 1.0f / float(base.height);
    return true;
}

// ============================================================================
//  Shaders y pipelines
// ============================================================================

VulkanRasterizer::FragShader* VulkanRasterizer::GetFragShader(u64 address, const SpirvOptions& opt) {
    std::vector<u32> key{opt.color_outputs, u32(opt.mrt), u32(opt.int64)};
    for (u8 k : opt.color_int) key.push_back(k);
    auto& mm = m_gpu.MemoryManager();
    auto& list = m_fragShaders[address];
    for (auto& fs : list) {
        if (fs->key != key) continue;
        // El mismo programa? (el juego puede subir otro en la misma direccion)
        std::vector<u8> now(fs->code.size());
        mm.ReadBlock(address, now.data(), now.size());
        if (now == fs->code) return fs.get();
        if (fs->module) { Submit(); vkDestroyShaderModule(m_vk, fs->module, nullptr); }
        fs->module = VK_NULL_HANDLE;
        fs->key.clear();   // ya no sirve
    }
    auto fs = std::make_unique<FragShader>();
    fs->address = address;
    fs->key = key;
    ShaderProgram prog(address, [&mm](u64 a, void* d, size_t n) { mm.ReadBlock(a, d, n); });
    fs->result = TranslateToSpirv(prog, opt);
    fs->code.resize(fs->result.ok ? fs->result.code_end : 0x50);
    mm.ReadBlock(address, fs->code.data(), fs->code.size());
    if (fs->result.ok) {
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = fs->result.code.size() * 4;
        smi.pCode = fs->result.code.data();
        if (vkCreateShaderModule(m_vk, &smi, nullptr, &fs->module) != VK_SUCCESS) {
            fs->result.ok = false;
            fs->result.error = "vkCreateShaderModule";
        }
        ++m_stats.shaders;
    }
    fs->id = m_nextShaderId++;
    list.erase(std::remove_if(list.begin(), list.end(), [](auto& p) { return p->key.empty(); }), list.end());
    list.push_back(std::move(fs));
    return list.back().get();
}

VkShaderModule VulkanRasterizer::GetVertexShader(const std::vector<SpirvVarying>& varyings) {
    std::vector<u32> key;
    for (const auto& v : varyings) key.push_back(u32(v.slot) | (u32(v.kind) << 8));
    auto it = m_vertShaders.find(key);
    if (it != m_vertShaders.end()) return it->second;
    const std::vector<u32> code = BuildPassthroughVertexShader(varyings);
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = code.size() * 4;
    smi.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(m_vk, &smi, nullptr, &m);
    m_vertShaders[key] = m;
    return m;
}

VkPipeline VulkanRasterizer::GetPipeline(const std::vector<u32>& key, const FragShader& fs, const PipelineDesc& d) {
    auto it = m_pipelines.find(key);
    if (it != m_pipelines.end()) return it->second;

    const VkShaderModule vs = GetVertexShader(d.varyings);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs.module;
    stages[1].pName = "main";

    // Vertices: posicion + un vec4 por varying, todo seguido
    const u32 stride = 16 * (1 + u32(d.varyings.size()));
    VkVertexInputBindingDescription vb{0, stride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attrs;
    attrs.push_back({0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    for (u32 i = 0; i < d.varyings.size(); ++i)
        attrs.push_back({i + 1, 0, d.varyings[i].kind == SpirvVarying::Flat ? VK_FORMAT_R32G32B32A32_UINT
                                                                            : VK_FORMAT_R32G32B32A32_SFLOAT, 16 * (i + 1)});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = u32(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull;
    rs.frontFace = d.front;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth_test;
    ds.depthWriteEnable = d.depth_write;
    ds.depthCompareOp = d.depth_op;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.ncolor;
    cb.pAttachments = d.blend;
    const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = d.ncolor;
    ri.pColorAttachmentFormats = d.color;
    ri.depthAttachmentFormat = d.depth;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &ri;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = m_pipeLayout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_vk, VK_NULL_HANDLE, 1, &gp, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    m_pipelines[key] = p;
    ++m_stats.pipelines;
    return p;
}

// ============================================================================
//  Draw
// ============================================================================

namespace {
// Un render target segun los registros (como el dibujo por software)
struct TargetRegs {
    Surface surf;
    u32 format = 0;
    VkFormat vk = VK_FORMAT_UNDEFINED;
    u8 kind = 0;
    u32 write_mask = 0xF;
    bool blend = false;
    u32 b[7] = {};
};
bool ReadTarget(const u32* regs, u32 k, TargetRegs& t) {
    const u32 ctl = regs[RT_CONTROL];
    const u32 index = (ctl >> (4 + 3 * k)) & 7;
    const u32 base = RT_BASE + index * 0x10;
    t.format = regs[base + 4];
    t.surf.address = Iova(regs[base], regs[base + 1]);
    t.surf.bytes_per_pixel = ColorFormatBytes(t.format);
    const u32 tile = regs[base + 5];
    t.surf.linear = (tile >> 12) & 1;
    if (t.surf.linear) {
        t.surf.pitch = regs[base + 2];
        t.surf.width = t.surf.bytes_per_pixel ? t.surf.pitch / t.surf.bytes_per_pixel : 0;
    } else {
        t.surf.width = regs[base + 2];
        t.surf.block_height_log2 = (tile >> 4) & 0xF;
    }
    t.surf.height = regs[base + 3];
    if (!t.surf.address || !t.surf.bytes_per_pixel || !t.surf.width || !t.surf.height) return false;
    const u32 wm = regs[CT_WRITE + ((regs[SINGLE_CT_WRITE] & 1) ? 0 : k)];
    t.write_mask = (wm & 1) | ((wm >> 3) & 2) | ((wm >> 6) & 4) | ((wm >> 9) & 8);
    t.blend = regs[BLEND_ENABLE + k] & 1;
    if (regs[BLEND_PER_TARGET] & 1) {
        for (u32 i = 0; i < 7; ++i) t.b[i] = regs[BLEND_TARGET + 8 * k + i];
    } else {
        const u32 g[7] = {regs[BLEND_GLOBAL], regs[BLEND_GLOBAL + 1], regs[BLEND_GLOBAL + 2], regs[BLEND_GLOBAL + 3],
                          regs[BLEND_GLOBAL + 4], regs[BLEND_GLOBAL + 5], regs[BLEND_GLOBAL + 7]};
        std::memcpy(t.b, g, sizeof(g));
    }
    return true;
}
bool ReadDepthTarget(const u32* regs, Surface& z, u32& format) {
    format = regs[ZT_FORMAT];
    z = {};
    z.address = Iova(regs[ZT_ADDR], regs[ZT_ADDR + 1]);
    z.bytes_per_pixel = DepthFormatBytes(format);
    z.width = regs[ZT_WIDTH];
    z.height = regs[ZT_HEIGHT];
    z.block_height_log2 = (regs[ZT_BLOCK] >> 4) & 0xF;
    float dummy;
    u8 zero[8] = {};
    return z.address && z.bytes_per_pixel && z.width && z.height && DecodeDepth(format, zero, dummy);
}
} // namespace

bool VulkanRasterizer::Draw(const u32* regs, const ConstbufTable& cbs, const DrawCall& dc) {
    Timer timer(m_stats.ns_draw);
    auto& mm = m_gpu.MemoryManager();
    auto& stats = m_gpu.GetStats();
    auto stage_enabled = [&](u32 s) { return regs[PIPELINE + 0x10 * s] & 1; };
    for (u32 s : {0u, 2u, 3u, 4u})
        if (stage_enabled(s)) return Fallback("teselado, geometria o VertexA");
    if (!stage_enabled(1) || !stage_enabled(5)) return false;
    if (dc.topology < 4 || dc.topology > 9) return false;   // puntos y lineas: el software avisa
    u32 msx, msy;
    MsaaSampleGrid(regs[MULTISAMPLE_MODE], msx, msy);
    if (msx * msy != 1) return Fallback("MSAA");

    const u64 region = Iova(regs[PROGRAM_REGION], regs[PROGRAM_REGION + 1]);
    const u64 fs_addr = region + regs[PIPELINE + 0x10 * 5 + 1];
    const u32 vs_group = std::min<u32>(regs[PIPELINE + 0x10 * 1 + 4] & 7, 4);
    u32 fs_group = std::min<u32>(regs[PIPELINE + 0x10 * 5 + 4] & 7, 4);
    if (vs_group == 0 && fs_group == 0) fs_group = 4;
    u32 fs_hdr0 = 0;
    mm.ReadBlock(fs_addr, &fs_hdr0, 4);
    const bool mrt = ((fs_hdr0 >> 14) & 1) && (regs[MRT_ENABLE] & 1);

    // --- Render targets y profundidad ---
    std::vector<TargetRegs> targets;
    const u32 num_targets = std::min<u32>(regs[RT_CONTROL] & 0xF, 8);
    for (u32 k = 0; k < num_targets; ++k) {
        TargetRegs t;
        if (!ReadTarget(regs, k, t)) continue;
        if (!ColorFormat(t.format, t.vk, t.kind)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "formato de color 0x%X", t.format);
            return Fallback(buf);
        }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(m_dev.Physical(), t.vk, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return Fallback("formato de color sin soporte en la GPU");
        if (t.kind != 0) t.blend = false;   // formatos enteros: sin mezcla (como el software)
        if (t.blend && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
            return Fallback("mezcla en un formato que la GPU no mezcla");
        targets.push_back(t);
    }
    const bool depth_test = regs[DEPTH_TEST] & 1, depth_write = regs[DEPTH_WRITE] & 1;
    Surface zsurf;
    u32 zformat = 0;
    const bool depth_enabled = (regs[ZT_SELECT] & 1) && (depth_test || depth_write) && ReadDepthTarget(regs, zsurf, zformat);
    if (targets.empty() && !depth_enabled) return false;

    // --- Zona dibujable (como el software) ---
    u32 fw = 0xFFFF, fh = 0xFFFF;
    for (const auto& t : targets) { fw = std::min(fw, t.surf.width); fh = std::min(fh, t.surf.height); }
    if (depth_enabled) { fw = std::min(fw, zsurf.width); fh = std::min(fh, zsurf.height); }
    u32 bx0 = 0, by0 = 0, bx1 = fw, by1 = fh;
    auto clip_rect = [&](u32 x0, u32 x1, u32 y0, u32 y1) {
        bx0 = std::max(bx0, x0); bx1 = std::min(bx1, x1);
        by0 = std::max(by0, y0); by1 = std::min(by1, y1);
    };
    const u32 vh = regs[VIEWPORT], vv = regs[VIEWPORT + 1];
    if (vh >> 16 && vv >> 16) clip_rect(vh & 0xFFFF, (vh & 0xFFFF) + (vh >> 16), vv & 0xFFFF, (vv & 0xFFFF) + (vv >> 16));
    const u32 sh = regs[SCREEN_SCISSOR_H], sv = regs[SCREEN_SCISSOR_V];
    if (sh >> 16 && sv >> 16) clip_rect(sh & 0xFFFF, (sh & 0xFFFF) + (sh >> 16), sv & 0xFFFF, (sv & 0xFFFF) + (sv >> 16));
    if (regs[SCISSOR] & 1) {
        const u32 h0 = regs[SCISSOR + 1], v0 = regs[SCISSOR + 2];
        clip_rect(h0 & 0xFFFF, h0 >> 16, v0 & 0xFFFF, v0 >> 16);
    }
    if (bx0 >= bx1 || by0 >= by1) return true;   // nada que dibujar

    // --- Shader de pixeles traducido ---
    SpirvOptions opt;
    opt.stage = SpirvOptions::Stage::Fragment;
    opt.int64 = m_dev.Features().shaderInt64;
    opt.color_outputs = u32(targets.size());
    opt.mrt = mrt;
    for (size_t k = 0; k < targets.size(); ++k) opt.color_int[k] = targets[k].kind;
    FragShader* fs = GetFragShader(fs_addr, opt);
    if (!fs->result.ok) return Fallback("shader de pixeles: " + fs->result.error);
    const auto& varyings = fs->result.varyings;
    const auto& lim = m_dev.Limits();
    if (varyings.size() + 1 > lim.maxVertexInputAttributes || varyings.size() * 4 > lim.maxFragmentInputComponents ||
        varyings.size() * 4 > lim.maxVertexOutputComponents)
        return Fallback("demasiadas entradas en el shader de pixeles");
    if (fs->result.textures.size() > MAX_TEXTURES) return Fallback("demasiadas texturas");

    // --- Vertices (shader de vertices en la CPU) ---
    const u64 tic_pool = Iova(regs[TIC_POOL], regs[TIC_POOL + 1]), tsc_pool = Iova(regs[TSC_POOL], regs[TSC_POOL + 1]);
    const u32 tex_cb = std::min<u32>(regs[BINDLESS_TEXTURE] & 0x1F, 17);
    SoftwareRasterizer::VertexStage vstage;
    {
        Timer t(m_stats.ns_vertex);
        TextureSampler vs_tex(m_gpu, tic_pool, regs[TIC_MAX], tsc_pool, regs[TSC_MAX], &m_gpu.Textures());
        SoftwareRasterizer::RunVertexStage(m_gpu, regs, cbs, dc, &vs_tex, tex_cb, vstage);
    }
    if (vstage.failed) {
        m_gpu.Warn("vs:" + vstage.error, vstage.error);
        ++stats.shader_errors;
        return true;
    }
    if (vstage.tris.empty()) return true;

    // --- Constbufs del shader de pixeles (copiados ahora: el programa puede cambiarlos despues) ---
    std::array<std::vector<u8>, 18> cbdata;
    for (u32 i = 0; i < 18; ++i) {
        if (!(fs->result.cbuf_mask & (1u << i))) continue;
        const ConstbufBinding& b = cbs[fs_group][i];
        if (b.valid && b.size) {
            cbdata[i].resize((std::min<u32>(b.size, 0x10000) + 3) & ~3u);
            mm.ReadBlock(b.address, cbdata[i].data(), cbdata[i].size());
        }
        if (cbdata[i].empty()) cbdata[i].assign(16, 0);
    }

    // --- Texturas ---
    std::vector<BoundTexture> bound(fs->result.textures.size());
    std::optional<Timer> ttex(std::in_place, m_stats.ns_texture);
    for (size_t k = 0; k < fs->result.textures.size(); ++k) {
        const SpirvTexture& t = fs->result.textures[k];
        const ConstbufBinding& b = cbs[fs_group][tex_cb];
        u32 handle = 0;
        if (b.valid && t.slot * 4 + 4 <= std::min<u32>(b.size, 0x10000)) handle = mm.Read<u32>(b.address + t.slot * 4);
        if (!PrepareTexture(handle, t, tic_pool, regs[TIC_MAX], tsc_pool, regs[TSC_MAX], bound[k])) return false;
    }
    ttex.reset();
    std::optional<Timer> timg(std::in_place, m_stats.ns_images);

    // --- Imagenes de los render targets (se suben si la memoria es mas nueva) ---
    std::vector<Image*> color_imgs;
    for (const auto& t : targets) {
        Image* im = GetImage(t.surf, t.format, false, false);
        if (!im) { return Fallback("no se pudo crear la imagen del render target"); }
        color_imgs.push_back(im);
    }
    Image* depth_img = nullptr;
    if (depth_enabled) {
        depth_img = GetImage(zsurf, zformat, true, false);
        if (!depth_img) { return Fallback("no se pudo crear la imagen de profundidad"); }
    }
    timg.reset();

    // --- Vertices para la GPU: posicion (ya con el viewport) + varyings ---
    const bool xform = regs[VIEWPORT_XFORM_ENABLE] & 1;
    const float sx = xform ? F(regs[VIEWPORT_XFORM]) : 1.0f, sy = xform ? F(regs[VIEWPORT_XFORM + 1]) : 1.0f;
    const float sz = xform ? F(regs[VIEWPORT_XFORM + 2]) : 1.0f, ox = xform ? F(regs[VIEWPORT_XFORM + 3]) : 0.0f;
    const float oy = xform ? F(regs[VIEWPORT_XFORM + 4]) : 0.0f, oz = xform ? F(regs[VIEWPORT_XFORM + 5]) : 0.0f;
    const u32 words = 4 * (1 + u32(varyings.size()));
    const u32 nverts = u32(vstage.tris.size()) * 3;
    std::vector<u32> vdata(size_t(nverts) * words);
    {
        u32* o = vdata.data();
        for (const auto& t : vstage.tris) {
            // El vertice que manda (valores "flat") va primero: Vulkan usa el primero
            u32 idx[3] = {t[0], t[1], t[2]};
            if (t[3] == t[1]) { idx[0] = t[1]; idx[1] = t[2]; idx[2] = t[0]; }
            else if (t[3] == t[2]) { idx[0] = t[2]; idx[1] = t[0]; idx[2] = t[1]; }
            for (u32 v : idx) {
                const auto& vx = vstage.verts[v];
                const float x = vx.Pos(0), y = vx.Pos(1), z = vx.Pos(2), w = vx.Pos(3);
                // Pantalla = escala * (pos / w) + desplazamiento; en coordenadas de recorte de un
                // viewport que cubre la zona [0, fw) x [0, fh), sin dividir por w:
                o[0] = U(2.0f * (sx * x + ox * w) / float(fw) - w);
                o[1] = U(2.0f * (sy * y + oy * w) / float(fh) - w);
                o[2] = U(sz * z + oz * w);
                o[3] = U(w);
                o += 4;
                for (const auto& vy : varyings) {
                    std::memcpy(o, &vx.attr[vy.slot * 4u], 16);
                    o += 4;
                }
            }
        }
    }

    // --- Espacio en el buffer de subida y descriptores (todo o nada: puede ejecutar lo pendiente) ---
    const VkDeviceSize salign = std::max<VkDeviceSize>(lim.minStorageBufferOffsetAlignment, 16);
    VkDeviceSize need = vdata.size() * 4 + 256 + salign * 20;
    for (const auto& c : cbdata) need += c.size();
    if (m_setsLeft == 0 || AlignUp(m_uploadOffset, 256) + need > m_upload.size) Submit();
    if (need > m_upload.size)
        EnsureBuffer(m_upload, AlignUp(need, 1 << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = m_descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(m_vk, &dai, &set) != VK_SUCCESS) return Fallback("sin descriptores");
    --m_setsLeft;
    const Alloc va = Allocate(vdata.size() * 4, 16);
    std::memcpy(va.ptr, vdata.data(), vdata.size() * 4);
    std::vector<VkDescriptorBufferInfo> binfo;
    std::vector<VkDescriptorImageInfo> iinfo;
    binfo.reserve(20);
    iinfo.reserve(MAX_TEXTURES);
    std::vector<VkWriteDescriptorSet> writes;
    auto write_buffer = [&](u32 binding, const void* data, size_t size) {
        const Alloc a = Allocate(size, salign);
        std::memcpy(a.ptr, data, size);
        binfo.push_back({m_upload.buffer, a.offset, size});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &binfo.back();
        writes.push_back(w);
    };
    for (u32 i = 0; i < 18; ++i)
        if (!cbdata[i].empty()) write_buffer(i, cbdata[i].data(), cbdata[i].size());
    u32 info[4 + 2 * MAX_TEXTURES] = {};
    info[0] = U((regs[WINDOW_ORIGIN] & 1) ? -1.0f : 1.0f);   // SR_Y_DIRECTION
    for (size_t k = 0; k < bound.size(); ++k) { info[4 + 2 * k] = U(bound[k].scale[0]); info[5 + 2 * k] = U(bound[k].scale[1]); }
    write_buffer(SpirvBinding::DRAW_INFO, info, sizeof(info));
    for (size_t k = 0; k < bound.size(); ++k) {
        iinfo.push_back({bound[k].sampler, bound[k].view, VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = SpirvBinding::TEXTURE0 + u32(k);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &iinfo.back();
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(m_vk, u32(writes.size()), writes.data(), 0, nullptr);

    // --- Pipeline ---
    PipelineDesc d;
    d.varyings = varyings;
    d.ncolor = u32(targets.size());
    std::vector<u32> key{u32(fs->id), u32(fs->id >> 32), d.ncolor};
    for (const auto& v : varyings) key.push_back(u32(v.slot) | (u32(v.kind) << 8));
    for (u32 k = 0; k < d.ncolor; ++k) {
        const TargetRegs& t = targets[k];
        d.color[k] = t.vk;
        VkPipelineColorBlendAttachmentState& bs = d.blend[k];
        bs.colorWriteMask = t.write_mask;
        bs.blendEnable = t.blend;
        if (t.blend) {
            const bool separate = t.b[0] & 1;
            bs.colorBlendOp = BlendOpVk(t.b[1]);
            bs.srcColorBlendFactor = BlendFactorVk(t.b[2]);
            bs.dstColorBlendFactor = BlendFactorVk(t.b[3]);
            bs.alphaBlendOp = separate ? BlendOpVk(t.b[4]) : bs.colorBlendOp;
            bs.srcAlphaBlendFactor = separate ? BlendFactorVk(t.b[5]) : bs.srcColorBlendFactor;
            bs.dstAlphaBlendFactor = separate ? BlendFactorVk(t.b[6]) : bs.dstColorBlendFactor;
        }
        key.insert(key.end(), {u32(t.vk), t.write_mask, u32(t.blend), u32(bs.colorBlendOp), u32(bs.srcColorBlendFactor),
                               u32(bs.dstColorBlendFactor), u32(bs.alphaBlendOp), u32(bs.srcAlphaBlendFactor),
                               u32(bs.dstAlphaBlendFactor)});
    }
    if (depth_enabled) {
        d.depth = VK_FORMAT_D32_SFLOAT;
        d.depth_test = true;   // (sin prueba pero escribiendo: "siempre")
        d.depth_op = depth_test ? CompareOpVk(regs[DEPTH_FUNC]) : VK_COMPARE_OP_ALWAYS;
        d.depth_write = depth_write;
    }
    if (regs[CULL_ENABLE] & 1) {
        const u32 cull = regs[CULL_FACE];
        d.cull = cull == 0x404 ? VK_CULL_MODE_FRONT_BIT : cull == 0x405 ? VK_CULL_MODE_BACK_BIT
               : cull == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    }
    // Sentido de la cara delantera (ver docs): con FlipY la pantalla esta al reves
    const bool ff_ccw = regs[FRONT_FACE] == 0x901, flip_y = (regs[WINDOW_ORIGIN] >> 4) & 1;
    d.front = ff_ccw != flip_y ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    key.insert(key.end(), {u32(d.depth), u32(d.depth_test), u32(d.depth_write), u32(d.depth_op), u32(d.cull), u32(d.front)});
    const VkPipeline pipe = GetPipeline(key, *fs, d);
    if (!pipe) return Fallback("no se pudo crear el pipeline");

    // --- Grabar ---
    bool same = m_inRendering && m_renderDepth == depth_img && m_renderColor == color_imgs;
    if (!same) {
        EndRendering();
        FullBarrier(Cmd());   // lo dibujado antes puede leerse ahora como textura
        m_lastOp = 2;
        std::vector<VkRenderingAttachmentInfo> ca(color_imgs.size(), {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO});
        for (size_t k = 0; k < color_imgs.size(); ++k) {
            ca[k].imageView = color_imgs[k]->view;
            ca[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca[k].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (depth_img) {
            da.imageView = depth_img->view;
            da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {fw, fh}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = u32(ca.size());
        ri.pColorAttachments = ca.data();
        ri.pDepthAttachment = depth_img ? &da : nullptr;
        m_dev.CmdBeginRendering(Cmd(), &ri);
        m_inRendering = true;
        m_renderColor = color_imgs;
        m_renderDepth = depth_img;
    }
    VkCommandBuffer cmd = Cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkViewport viewport{0.0f, 0.0f, float(fw), float(fh), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{{s32(bx0), s32(by0)}, {bx1 - bx0, by1 - by0}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const float bc[4] = {F(regs[BLEND_CONST]), F(regs[BLEND_CONST + 1]), F(regs[BLEND_CONST + 2]), F(regs[BLEND_CONST + 3])};
    vkCmdSetBlendConstants(cmd, bc);
    const VkDeviceSize voff = va.offset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_upload.buffer, &voff);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLayout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, nverts, 1, 0, 0);

    for (Image* im : color_imgs) {
        im->rendered = true;
        if (!im->gpu_dirty) { im->gpu_dirty = true; ++m_dirtyCount; }
    }
    if (depth_img && depth_write && !depth_img->gpu_dirty) { depth_img->gpu_dirty = true; ++m_dirtyCount; }
    ++m_stats.draws;
    ++stats.draws;
    ++stats.draws_hw;
    stats.triangles += vstage.tris.size();
    return true;
}

// ============================================================================
//  Borrados
// ============================================================================

bool VulkanRasterizer::Clear(const u32* regs, u32 arg) {
    const bool clear_z = arg & 1, clear_s = (arg >> 1) & 1;
    const u32 comp_mask = (arg >> 2) & 0xF;
    const u32 target = (arg >> 6) & 0xF;
    const u32 layer = (arg >> 10) & 0x7FF;
    u32 msx, msy;
    MsaaSampleGrid(regs[MULTISAMPLE_MODE], msx, msy);
    if (msx * msy != 1) return false;
    if (clear_s && !clear_z) return false;   // solo stencil: no lo guardamos en la GPU

    // Zona: como Maxwell3D::ClearBuffers
    auto clip = [&](u32 w, u32 h, u32& x0, u32& y0, u32& x1, u32& y1) {
        x0 = 0; y0 = 0; x1 = w; y1 = h;
        const u32 sh = regs[SCREEN_SCISSOR_H], sv = regs[SCREEN_SCISSOR_V];
        if (sh >> 16) { x0 = std::max(x0, sh & 0xFFFF); x1 = std::min(x1, (sh & 0xFFFF) + (sh >> 16)); }
        if (sv >> 16) { y0 = std::max(y0, sv & 0xFFFF); y1 = std::min(y1, (sv & 0xFFFF) + (sv >> 16)); }
        if (((regs[CLEAR_FLAGS] >> 8) & 1) && (regs[SCISSOR] & 1)) {
            const u32 h0 = regs[SCISSOR + 1], v0 = regs[SCISSOR + 2];
            x0 = std::max(x0, h0 & 0xFFFF); x1 = std::min(x1, h0 >> 16);
            y0 = std::max(y0, v0 & 0xFFFF); y1 = std::min(y1, v0 >> 16);
        }
    };
    auto existing = [&](const Surface& s) {
        for (auto& im : m_images) if (im->surf.address == s.address) return true;
        return false;
    };

    // Primero ver si se puede todo; si no, nada (lo hace el software)
    Surface rt;
    u32 rt_format = 0;
    VkFormat rt_vk = VK_FORMAT_UNDEFINED;
    u8 kind = 0;
    u32 cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    bool do_color = false, color_full = false;
    if (comp_mask && target < 8) {
        const u32 base = RT_BASE + target * 0x10;
        rt_format = regs[base + 4];
        rt.address = Iova(regs[base], regs[base + 1]) + u64(layer) * u64(regs[base + 7]) * 4;
        rt.bytes_per_pixel = ColorFormatBytes(rt_format);
        const u32 tile = regs[base + 5];
        rt.linear = (tile >> 12) & 1;
        if (rt.linear) { rt.pitch = regs[base + 2]; rt.width = rt.bytes_per_pixel ? rt.pitch / rt.bytes_per_pixel : 0; }
        else { rt.width = regs[base + 2]; rt.block_height_log2 = (tile >> 4) & 0xF; }
        rt.height = regs[base + 3];
        if (!rt.address || !rt.bytes_per_pixel || !rt.width || !rt.height) return false;
        if (!ColorFormat(rt_format, rt_vk, kind)) return false;
        if (ColorWriteByteMask(rt_format, comp_mask) != (1u << rt.bytes_per_pixel) - 1) return false;   // solo algunos canales
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(m_dev.Physical(), rt_vk, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return false;
        clip(rt.width, rt.height, cx0, cy0, cx1, cy1);
        color_full = cx0 == 0 && cy0 == 0 && cx1 == rt.width && cy1 == rt.height;
        if (!color_full && !existing(rt)) return false;   // poca cosa y no esta en la GPU: software
        do_color = cx1 > cx0 && cy1 > cy0;
    }
    Surface zt;
    u32 zformat = 0;
    u32 zx0 = 0, zy0 = 0, zx1 = 0, zy1 = 0;
    bool do_depth = false, depth_full = false;
    if (clear_z && (regs[ZT_SELECT] & 1)) {
        if (!ReadDepthTarget(regs, zt, zformat)) return false;
        zt.address += u64(layer) * u64(regs[ZT_LAYER_SIZE]) * 4;
        clip(zt.width, zt.height, zx0, zy0, zx1, zy1);
        depth_full = zx0 == 0 && zy0 == 0 && zx1 == zt.width && zy1 == zt.height;
        // Con stencil en el formato, borrar la profundidad a medias deja el stencil viejo: bien
        if (!depth_full && !existing(zt)) return false;
        do_depth = zx1 > zx0 && zy1 > zy0;
    }
    if (!do_color && !do_depth) return true;

    auto clear_image = [&](Image* im, u32 x0, u32 y0, u32 x1, u32 y1, const VkClearAttachment& ca) {
        EndRendering();
        RenderBarrier();
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = im->view;
        att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {im->surf.width, im->surf.height}};
        ri.layerCount = 1;
        if (im->depth) ri.pDepthAttachment = &att;
        else { ri.colorAttachmentCount = 1; ri.pColorAttachments = &att; }
        m_dev.CmdBeginRendering(Cmd(), &ri);
        VkClearRect rect{{{s32(x0), s32(y0)}, {x1 - x0, y1 - y0}}, 0, 1};
        vkCmdClearAttachments(Cmd(), 1, &ca, 1, &rect);
        m_dev.CmdEndRendering(Cmd());
        im->rendered = true;
        if (!im->gpu_dirty) { im->gpu_dirty = true; ++m_dirtyCount; }
    };
    if (do_color) {
        Image* im = GetImage(rt, rt_format, false, color_full);
        if (!im) return false;
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        float c[4];
        std::memcpy(c, &regs[CLEAR_COLOR], 16);
        for (int i = 0; i < 4; ++i) {
            if (kind == 1) ca.clearValue.color.int32[i] = s32(c[i]);
            else if (kind == 2) ca.clearValue.color.uint32[i] = u32(std::max(c[i], 0.0f));
            else ca.clearValue.color.float32[i] = c[i];
        }
        clear_image(im, cx0, cy0, cx1, cy1, ca);
    }
    if (do_depth) {
        Image* im = GetImage(zt, zformat, true, depth_full);
        if (!im) return false;
        VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
        ca.clearValue.depthStencil.depth = std::clamp(F(regs[CLEAR_DEPTH]), 0.0f, 1.0f);
        clear_image(im, zx0, zy0, zx1, zy1, ca);
    }
    ++m_stats.clears;
    ++m_gpu.GetStats().clears;
    return true;
}

bool VulkanRasterizer::Blit2D(const Surface& src, u32 src_format, const Surface& dst, u32 dst_format, u32 dx0, u32 dy0,
                              u32 w, u32 h, s64 u0, s64 v0, s64 dudx, s64 dvdy) {
    // Solo si el origen ya esta en la GPU (si no, el software lo hace igual de bien)
    Image* sim = nullptr;
    for (auto& im : m_images)
        if (!im->depth && im->surf.address == src.address && im->guest_format == src_format && im->surf.width == src.width &&
            im->surf.height == src.height && im->surf.linear == src.linear && im->surf.block_height_log2 == src.block_height_log2)
            sim = im.get();
    if (!sim || src.address == dst.address) return false;
    VkFormat dvk;
    u8 dkind, skind;
    VkFormat svk;
    if (!ColorFormat(dst_format, dvk, dkind) || !ColorFormat(src_format, svk, skind) || dkind != skind) return false;
    if (dst.bytes_per_pixel != ColorFormatBytes(dst_format) || !dst.width || !dst.height) return false;
    VkFormatProperties sp, dp;
    vkGetPhysicalDeviceFormatProperties(m_dev.Physical(), svk, &sp);
    vkGetPhysicalDeviceFormatProperties(m_dev.Physical(), dvk, &dp);
    if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return false;
    // Zona de origen: el software toma el texel floor(u0 + x * dudx); con dudx negativo la
    // zona va al reves (Vulkan la recorre de mayor a menor)
    auto range = [](s64 start, s64 step, u32 n, s32& a, s32& b) {
        a = s32(start >> 32);
        b = s32((start + step * s64(n)) >> 32);
        if (step < 0) { ++a; ++b; }
    };
    s32 sx0, sx1, sy0, sy1;
    range(u0, dudx, w, sx0, sx1);
    range(v0, dvdy, h, sy0, sy1);
    auto inside = [](s32 v, u32 max) { return v >= 0 && v <= s32(max); };
    if (!inside(sx0, src.width) || !inside(sx1, src.width) || !inside(sy0, src.height) || !inside(sy1, src.height)) return false;
    const u32 cw = std::min(w, dst.width > dx0 ? dst.width - dx0 : 0), ch = std::min(h, dst.height > dy0 ? dst.height - dy0 : 0);
    if (cw != w || ch != h) return false;   // se sale del destino: software

    Image* src_img = GetImage(sim->surf, sim->guest_format, false, false);
    const bool full = dx0 == 0 && dy0 == 0 && w == dst.width && h == dst.height;
    Image* dst_img = GetImage(dst, dst_format, false, full);
    if (!src_img || !dst_img) return false;
    TransferBarrier();
    VkImageBlit r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.srcOffsets[0] = {sx0, sy0, 0};
    r.srcOffsets[1] = {sx1, sy1, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstOffsets[0] = {s32(dx0), s32(dy0), 0};
    r.dstOffsets[1] = {s32(dx0 + w), s32(dy0 + h), 1};
    vkCmdBlitImage(Cmd(), src_img->image, VK_IMAGE_LAYOUT_GENERAL, dst_img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_NEAREST);
    if (!dst_img->gpu_dirty) { dst_img->gpu_dirty = true; ++m_dirtyCount; }
    ++m_stats.blits;
    return true;
}

void VulkanRasterizer::PrepareSoftwareDraw(const u32* regs) {
    if (m_dirtyCount == 0) return;
    // El dibujo por software lee y escribe sus render targets con punteros directos (sin
    // pasar por ReadBlock): lo que la GPU tenga de ellos tiene que estar ya en memoria
    const u32 num_targets = std::min<u32>(regs[RT_CONTROL] & 0xF, 8);
    for (u32 k = 0; k < num_targets; ++k) {
        TargetRegs t;
        if (ReadTarget(regs, k, t)) FlushRegion(t.surf.address, t.surf.SizeBytes());
    }
    Surface z;
    u32 zformat;
    if ((regs[ZT_SELECT] & 1) && ReadDepthTarget(regs, z, zformat)) FlushRegion(z.address, z.SizeBytes());
}

} // namespace NeXo2::GPU
