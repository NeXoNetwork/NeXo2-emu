#pragma once
// Vulkan (GPU fase 3): el dispositivo. Abre Vulkan sin ventana, elige la tarjeta grafica
// y crea una cola para dibujar y calcular. Las funciones de Vulkan se cargan al arrancar
// con volk (externals/volk), asi que no hace falta instalar el Vulkan SDK: basta con el
// driver de la tarjeta grafica.
//
// Ver docs/07-nexo-internals/gpu-vulkan.md.
#include <memory>
#include <string>
#include <vector>
#include "common/types.hpp"
#include "volk.h"

namespace NeXo2::GPU::Vulkan {

// Un buffer de Vulkan con su memoria (visible desde la CPU si 'host_visible')
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;   // direccion en la CPU (solo host_visible)
};

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Abre Vulkan. false si no hay driver o ninguna GPU sirve (el motivo en Error()).
    // validation = activar las capas de validacion si estan instaladas (para depurar).
    bool Init(bool validation = false);
    bool Ok() const { return m_device != VK_NULL_HANDLE; }
    const std::string& Error() const { return m_error; }

    // Datos para la interfaz
    const std::string& DeviceName() const { return m_name; }
    std::string ApiVersionString() const;
    std::string DriverString() const { return m_driver; }
    bool IsSoftware() const { return m_type == VK_PHYSICAL_DEVICE_TYPE_CPU; }   // lavapipe, SwiftShader...

    VkInstance Instance() const { return m_instance; }
    VkPhysicalDevice Physical() const { return m_physical; }
    VkDevice Handle() const { return m_device; }
    VkQueue Queue() const { return m_queue; }
    u32 QueueFamily() const { return m_queueFamily; }
    const VkPhysicalDeviceLimits& Limits() const { return m_props.limits; }

    // Memoria: tipo que cumpla 'bits' (de vkGet*MemoryRequirements) y 'props'. -1 si no hay.
    s32 FindMemoryType(u32 bits, VkMemoryPropertyFlags props) const;
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void DestroyBuffer(Buffer& b);

    // Graba comandos en 'record' y los ejecuta ya, esperando a que terminen
    template <typename F> bool Submit(F&& record) {
        VkCommandBuffer cmd = BeginOneShot();
        if (!cmd) return false;
        record(cmd);
        return EndOneShot(cmd);
    }

private:
    VkCommandBuffer BeginOneShot();
    bool EndOneShot(VkCommandBuffer cmd);
    bool Fail(const std::string& what, VkResult r = VK_SUCCESS);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    u32 m_queueFamily = 0;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_props{};
    VkPhysicalDeviceMemoryProperties m_memProps{};
    VkPhysicalDeviceType m_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::string m_name, m_driver, m_error;
};

// Texto de un VkResult ("VK_ERROR_OUT_OF_DEVICE_MEMORY"...)
const char* ResultName(VkResult r);

} // namespace NeXo2::GPU::Vulkan
