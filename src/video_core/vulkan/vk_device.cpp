// Dispositivo Vulkan (ver vk_device.hpp)
#include "vk_device.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace NeXo2::GPU::Vulkan {

using Common::Logger;

const char* ResultName(VkResult r) {
    switch (r) {
#define R(x) case x: return #x;
        R(VK_SUCCESS) R(VK_NOT_READY) R(VK_TIMEOUT) R(VK_INCOMPLETE)
        R(VK_ERROR_OUT_OF_HOST_MEMORY) R(VK_ERROR_OUT_OF_DEVICE_MEMORY) R(VK_ERROR_INITIALIZATION_FAILED)
        R(VK_ERROR_DEVICE_LOST) R(VK_ERROR_MEMORY_MAP_FAILED) R(VK_ERROR_LAYER_NOT_PRESENT)
        R(VK_ERROR_EXTENSION_NOT_PRESENT) R(VK_ERROR_FEATURE_NOT_PRESENT) R(VK_ERROR_INCOMPATIBLE_DRIVER)
        R(VK_ERROR_TOO_MANY_OBJECTS) R(VK_ERROR_FORMAT_NOT_SUPPORTED)
#undef R
        default: return "VK_ERROR_?";
    }
}

namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    Logger::Log(error ? Logger::Level::Error : Logger::Level::Warning,
                std::string("[Vulkan] ") + (data && data->pMessage ? data->pMessage : "?"));
    return VK_FALSE;
}

bool HasLayer(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (const auto& l : layers) if (!std::strcmp(l.layerName, name)) return true;
    return false;
}
bool HasInstanceExtension(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, ext.data());
    for (const auto& e : ext) if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}
int TypeScore(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
        default: return 0;
    }
}
} // namespace

bool Device::Fail(const std::string& what, VkResult r) {
    m_error = what + (r != VK_SUCCESS ? std::string(" (") + ResultName(r) + ")" : "");
    Logger::Log(Logger::Level::Warning, "[Vulkan] " + m_error);
    return false;
}

bool Device::Init(bool validation) {
    if (Ok()) return true;
    // 1. Cargar la biblioteca de Vulkan del sistema (vulkan-1.dll / libvulkan.so.1)
    if (VkResult r = volkInitialize(); r != VK_SUCCESS) return Fail("no hay Vulkan en este PC (falta el driver)", r);

    // 2. Instancia
    u32 api = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&api);
    api = std::min<u32>(api, VK_API_VERSION_1_3);
    if (api < VK_API_VERSION_1_1) return Fail("el driver de Vulkan es muy antiguo (hace falta 1.1)");

    std::vector<const char*> layers, extensions;
    const bool use_validation = validation && HasLayer("VK_LAYER_KHRONOS_validation");
    if (validation && !use_validation)
        Logger::Log(Logger::Level::Warning, "[Vulkan] capas de validacion pedidas pero no instaladas");
    if (use_validation) layers.push_back("VK_LAYER_KHRONOS_validation");
    const bool debug_utils = use_validation && HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "NeXo 2";
    app.pEngineName = "NeXo 2";
    app.apiVersion = api;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = u32(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = u32(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &m_instance); r != VK_SUCCESS) return Fail("vkCreateInstance", r);
    volkLoadInstance(m_instance);

    if (debug_utils) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(m_instance, &mci, nullptr, &m_messenger);
    }

    // 3. Elegir la GPU: dedicada > integrada > virtual > por software. NEXO2_VK_DEVICE=n fuerza una.
    u32 count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
    if (devices.empty()) return Fail("Vulkan no encuentra ninguna GPU");
    const char* forced = std::getenv("NEXO2_VK_DEVICE");
    int best_score = -1;
    for (u32 i = 0; i < devices.size(); ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qn, qf.data());
        s32 family = -1;
        for (u32 q = 0; q < qn; ++q)
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) { family = s32(q); break; }
        Logger::Log(Logger::Level::Info, "[Vulkan] GPU " + std::to_string(i) + ": " + p.deviceName +
                                             (family < 0 ? " (sin cola de graficos, no sirve)" : ""));
        if (family < 0 || p.apiVersion < VK_API_VERSION_1_1) continue;
        int score = TypeScore(p.deviceType) * 10;
        if (forced) score = (std::atoi(forced) == int(i)) ? 1000 : 0;
        if (score > best_score) {
            best_score = score;
            m_physical = devices[i];
            m_queueFamily = u32(family);
        }
    }
    if (!m_physical) return Fail("ninguna GPU tiene lo necesario (Vulkan 1.1 y una cola de graficos)");
    vkGetPhysicalDeviceProperties(m_physical, &m_props);
    vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memProps);
    m_type = m_props.deviceType;
    m_name = m_props.deviceName;
    {
        char buf[64];
        const u32 d = m_props.driverVersion;
        if (m_props.vendorID == 0x10DE)   // NVIDIA: 10.8.8.6 bits
            std::snprintf(buf, sizeof(buf), "%u.%u", d >> 22, (d >> 14) & 0xFF);
        else
            std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_API_VERSION_MAJOR(d), VK_API_VERSION_MINOR(d), VK_API_VERSION_PATCH(d));
        m_driver = buf;
    }

    // 4. Dispositivo logico con una cola
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_physical, &supported);
    VkPhysicalDeviceFeatures features{};   // solo lo que hay y nos sirve
    features.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    features.shaderInt64 = supported.shaderInt64;
    features.independentBlend = supported.independentBlend;
    features.depthClamp = supported.depthClamp;
    features.samplerAnisotropy = supported.samplerAnisotropy;
    features.textureCompressionBC = supported.textureCompressionBC;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &features;
    if (VkResult r = vkCreateDevice(m_physical, &dci, nullptr, &m_device); r != VK_SUCCESS) {
        m_device = VK_NULL_HANDLE;
        return Fail("vkCreateDevice", r);
    }
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_queueFamily;
    vkCreateCommandPool(m_device, &pci, nullptr, &m_pool);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(m_device, &fci, nullptr, &m_fence);

    Logger::Log(Logger::Level::Info, "[Vulkan] Usando " + m_name + " (Vulkan " + ApiVersionString() + ", driver " +
                                         m_driver + (use_validation ? ", con validacion" : "") + ")");
    return true;
}

Device::~Device() {
    if (m_device) {
        vkDeviceWaitIdle(m_device);
        if (m_fence) vkDestroyFence(m_device, m_fence, nullptr);
        if (m_pool) vkDestroyCommandPool(m_device, m_pool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_messenger) vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

std::string Device::ApiVersionString() const {
    const u32 v = m_props.apiVersion;
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

s32 Device::FindMemoryType(u32 bits, VkMemoryPropertyFlags props) const {
    for (u32 i = 0; i < m_memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m_memProps.memoryTypes[i].propertyFlags & props) == props) return s32(i);
    return -1;
}

Buffer Device::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    Buffer b;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &b.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, b.buffer, &req);
    const VkMemoryPropertyFlags want = host_visible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    s32 type = FindMemoryType(req.memoryTypeBits, want);
    if (type < 0 && !host_visible) type = FindMemoryType(req.memoryTypeBits, 0);
    if (type < 0) { vkDestroyBuffer(m_device, b.buffer, nullptr); return {}; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = u32(type);
    if (vkAllocateMemory(m_device, &mai, nullptr, &b.memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, b.buffer, nullptr);
        return {};
    }
    vkBindBufferMemory(m_device, b.buffer, b.memory, 0);
    if (host_visible) vkMapMemory(m_device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    b.size = size;
    return b;
}

void Device::DestroyBuffer(Buffer& b) {
    if (!m_device) return;
    if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    b = {};
}

VkCommandBuffer Device::BeginOneShot() {
    if (!m_device) return VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

bool Device::EndOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkResetFences(m_device, 1, &m_fence);
    VkResult r = vkQueueSubmit(m_queue, 1, &si, m_fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, 10'000'000'000ull);
    vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
    if (r != VK_SUCCESS) return Fail("vkQueueSubmit", r);
    return true;
}

} // namespace NeXo2::GPU::Vulkan
