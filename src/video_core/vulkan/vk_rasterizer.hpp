#pragma once
// Dibujo con Vulkan (GPU fase 3c): hace en la tarjeta grafica lo mismo que el dibujo por
// software (rasterizer.hpp).
//
//   - El shader de vertices de Maxwell se sigue ejecutando en la CPU (son pocos vertices).
//     La GPU recibe sus resultados con un shader de vertices "de paso".
//   - El shader de pixeles se traduce a SPIR-V (shader_spirv.hpp) y lo ejecuta la GPU. Es
//     lo caro: millones de pixeles por segundo.
//   - Los render targets y la profundidad son imagenes de Vulkan. La memoria del programa
//     sigue siendo la referencia: una imagen se sube desde ella la primera vez (o si alguien
//     la ha cambiado) y se escribe de vuelta al terminar cada envio de comandos, o antes si
//     otra parte de la GPU (copias, 2D, texturas, dibujo por software) va a leer esa memoria.
//   - Las texturas se decodifican en la CPU (la cache de texture.hpp) y se suben como
//     imagenes con sus mipmaps; el muestreo (filtros, repeticion, mipmaps) lo hace Vulkan.
//
// Lo que aun no sabe hacer (y entonces ese draw se hace por software): MSAA, texturas con
// comparacion de profundidad o de formato entero, algunas instrucciones de shader.
//
// Ver docs/07-nexo-internals/gpu-vulkan.md.
#include <array>
#include <map>
#include <unordered_set>
#include <memory>
#include <string>
#include <vector>
#include "vk_device.hpp"
#include "video_core/gpu.hpp"
#include "video_core/rasterizer.hpp"
#include "video_core/shader_spirv.hpp"
#include "video_core/surface.hpp"
#include "video_core/texture.hpp"

namespace NeXo2::GPU {

class VulkanRasterizer final : public GpuAccessHook {
public:
    VulkanRasterizer(Gpu& gpu, Vulkan::Device& device);
    ~VulkanRasterizer() override;
    VulkanRasterizer(const VulkanRasterizer&) = delete;
    VulkanRasterizer& operator=(const VulkanRasterizer&) = delete;

    // Crea lo comun (layouts, buffers). false si la GPU no sirve (motivo en 'error').
    bool Init(std::string& error);

    // Dibuja con el estado del motor 3D. true = hecho (o no habia nada que dibujar);
    // false = este draw tiene que hacerse por software (motivo en LastFallback()).
    bool Draw(const u32* regs, const ConstbufTable& cbs, const DrawCall& dc);
    // ClearSurface del motor 3D. true = hecho en la GPU; false = hacerlo por software.
    bool Clear(const u32* regs, u32 arg);
    // Copia del motor 2D (coordenadas de origen en punto fijo 32.32). true = hecha en la GPU;
    // false = la hace el software (solo se hace aqui si el origen ya es una imagen de la GPU)
    bool Blit2D(const Surface& src, u32 src_format, const Surface& dst, u32 dst_format, u32 dx0, u32 dy0, u32 w, u32 h,
                s64 u0, s64 v0, s64 dudx, s64 dvdy);
    // Antes de un draw por software: escribe en memoria sus render targets si estan en la GPU
    void PrepareSoftwareDraw(const u32* regs);

    // Imagen para presentar leida directamente del render target (ver Gpu::ReadForPresent)
    bool ReadForPresent(u64 va, u32 width, u32 height, u32 format, std::vector<u32>& rgba);
    // Termina el trabajo pendiente y escribe en memoria las imagenes cambiadas (las de
    // profundidad solo si 'depth')
    void Flush(bool depth = true);
    // Igual, solo las imagenes que tocan [va, va+size)
    void FlushRegion(u64 va, u64 size);
    // Otra parte de la GPU escribio [va, va+size): las imagenes de esa zona se vuelven a subir
    void OnGpuWrite(u64 va, u64 size);
    // Nuevo envio de comandos: la CPU puede haber cambiado la memoria
    void NewEpoch() { ++m_epoch; }
    // Olvidar todo (al reiniciar la GPU), sin escribir nada
    void Reset();

    // GpuAccessHook: alguien va a leer o escribir la memoria de la GPU
    void BeforeAccess(u64 va, u64 size, bool write) override;

    struct Stats {
        u64 draws = 0, clears = 0, fallbacks = 0, uploads = 0, downloads = 0, submits = 0;
        u64 pipelines = 0, shaders = 0, textures = 0;
        u64 rt_textures = 0;
        u64 blits = 0;         // copias del motor 2D hechas en la GPU
        u64 presents = 0;
        u64 tex_uploads = 0;   // niveles de textura subidos      // imagenes presentadas directamente desde la GPU
        u64 ns_present = 0;   // texturas leidas directamente de un render target
        // Tiempo (ns) de cada parte, en el hilo de la GPU: para ver donde se va
        u64 ns_draw = 0;       // Draw entero
        u64 ns_vertex = 0;     // shader de vertices en la CPU
        u64 ns_texture = 0;    // preparar texturas (decodificar y subir)
        u64 ns_images = 0;     // render targets: comprobar hash y subir
        u64 ns_wait = 0;       // esperando a la GPU (Submit)
        u64 ns_download = 0;   // escribir imagenes en memoria (sin la espera)
    };
    const Stats& GetStats() const { return m_stats; }
    const std::string& LastFallback() const { return m_lastFallback; }

private:
    // ---- Imagenes de render targets / profundidad ----
    struct Image {
        Surface surf;
        u32 guest_format = 0;
        bool depth = false;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool gpu_dirty = false;   // la imagen tiene cosas que la memoria aun no
        bool mem_dirty = true;    // la memoria tiene cosas que la imagen aun no
        u64 hash = 0;             // hash de la memoria la ultima vez que coincidian
        bool hash_known = false;  // false: borrada/copiada entera en la GPU sin pasar por memoria;
                                  // lo que haya en memoria es viejo y no cuenta como cambio de la CPU
        u64 epoch = 0;            // envio en el que se comprobo el hash
        u64 lo = 0, hi = 0;       // direcciones de la GPU que ocupa
        std::map<u32, VkImageView> sampled;
        bool rendered = false;    // se ha dibujado o borrado (puede presentarse); si solo se
                                  // copia con el 2D, no se escribe en memoria al final del envio   // vistas para usarla como textura (por swizzle)
    };
    Image* GetImage(const Surface& surf, u32 guest_format, bool depth, bool will_overwrite);
    bool CreateImage(Image& img);
    void DestroyImage(Image& img);
    void Upload(Image& img);
    void Download(const std::vector<Image*>& imgs);
    void FlushImages(const std::vector<Image*>& imgs);

    // ---- Texturas ----
    struct Texture {
        std::array<u32, 8> tic{};
        u8 dim = 1;
        bool array = false;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        u32 width = 0, height = 0, depth = 0, levels = 0;
        bool native = false;      // en su formato original (si no, decodificada a 4 floats)
        std::vector<std::shared_ptr<const DecodedLevel>> uploaded;   // (decodificada)
        struct LevelState {       // (nativa) lo que se subio de cada nivel
            bool valid = false, dirty = false;
            u64 hash = 0, quick = 0, epoch = 0, full_epoch = 0;
        };
        std::vector<LevelState> lv;
        u64 lo = ~0ull, hi = 0;   // memoria que ocupa (para los avisos de escritura)
    };
    // Formato de Vulkan para subir la textura tal cual; false = decodificarla en la CPU
    bool NativeTextureFormat(const TextureInfo& info, VkFormat& fmt, bool& alpha_one);
    std::map<VkFormat, bool> m_formatOk;
    std::unordered_set<u64> m_texPages;   // paginas de 64 KB con texturas nativas
    struct BoundTexture { VkImageView view; VkSampler sampler; float scale[2]; };
    bool PrepareTexture(u32 handle, const SpirvTexture& want, u64 tic_pool, u32 tic_max, u64 tsc_pool, u32 tsc_max,
                        BoundTexture& out);
    VkSampler GetSampler(const SamplerInfo& s, float tic_bias, bool no_mips);
    // Si la textura es un render target que esta en la GPU, su imagen (sin pasar por memoria)
    Image* RenderTargetAsTexture(const TextureInfo& info, const SpirvTexture& want);

    // ---- Shaders y pipelines ----
    struct FragShader {
        u64 address = 0;
        std::vector<u32> key;           // opciones de la traduccion
        std::vector<u8> code;           // bytes del programa (para ver si ha cambiado)
        SpirvResult result;
        VkShaderModule module = VK_NULL_HANDLE;
        u64 id = 0;
    };
    FragShader* GetFragShader(u64 address, const SpirvOptions& opt);
    VkShaderModule GetVertexShader(const std::vector<SpirvVarying>& varyings);
    VkPipeline GetPipeline(const std::vector<u32>& key, const FragShader& fs, const struct PipelineDesc& desc);

    // ---- Comandos ----
    VkCommandBuffer Cmd();             // el buffer de comandos abierto (lo abre si hace falta)
    void Submit();                     // ejecuta lo grabado y espera
    void EndRendering();
    void TransferBarrier();            // antes de copias (despues de dibujar)
    void RenderBarrier();              // antes de dibujar (despues de copias)
    // Espacio en el buffer de subida (memoria visible desde la CPU); puede llamar a Submit
    struct Alloc { VkDeviceSize offset; u8* ptr; };
    Alloc Allocate(VkDeviceSize size, VkDeviceSize align);
    bool EnsureBuffer(Vulkan::Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage);
    bool Fallback(const std::string& why);

    Gpu& m_gpu;
    Vulkan::Device& m_dev;
    VkDevice m_vk = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    bool m_recording = false;
    VkFence m_fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    u32 m_setsLeft = 0;
    Vulkan::Buffer m_upload, m_readback;
    VkDeviceSize m_uploadOffset = 0;
    VkFormat m_texFormat = VK_FORMAT_R32G32B32A32_SFLOAT;   // formato de las texturas subidas

    // Estado de la grabacion
    bool m_inRendering = false;
    std::vector<Image*> m_renderColor;
    Image* m_renderDepth = nullptr;
    int m_lastOp = 0;                  // 0 nada, 1 copia, 2 dibujo

    std::vector<std::unique_ptr<Image>> m_images;
    std::map<std::vector<u32>, std::unique_ptr<Texture>> m_textures;
    std::map<std::vector<u32>, VkSampler> m_samplers;
    std::map<u64, std::vector<std::unique_ptr<FragShader>>> m_fragShaders;
    std::map<std::vector<u32>, VkShaderModule> m_vertShaders;
    // Shaders de vertices ya decodificados (se ejecutan en la CPU), por direccion
    std::map<u64, std::unique_ptr<ShaderProgram>> m_vsPrograms;
    std::map<std::vector<u32>, VkPipeline> m_pipelines;
    u64 m_nextShaderId = 1;
    u64 m_epoch = 1;
    u32 m_dirtyCount = 0;              // imagenes con gpu_dirty (atajo de BeforeAccess)
    bool m_busy = false;               // dentro de una subida/bajada: no volver a entrar
    Stats m_stats;
    std::string m_lastFallback;
};

} // namespace NeXo2::GPU
