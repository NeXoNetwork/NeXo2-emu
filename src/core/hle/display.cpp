#include "display.hpp"
#include <cstring>

namespace NeXo2::HLE {

namespace {
// Formatos de pixel de Android que usa libnx
constexpr u32 PIXEL_FORMAT_RGBA_8888 = 1;
constexpr u32 PIXEL_FORMAT_RGBX_8888 = 2;
constexpr u32 PIXEL_FORMAT_RGB_565   = 4;
constexpr u32 PIXEL_FORMAT_BGRA_8888 = 5;
constexpr u32 PIXEL_FORMAT_RGBA_4444 = 7;

constexpr u32 LAYOUT_PITCH        = 1;
constexpr u32 LAYOUT_BLOCK_LINEAR = 3;

u32 BytesPerPixel(u32 format) {
    switch (format) {
        case PIXEL_FORMAT_RGB_565:
        case PIXEL_FORMAT_RGBA_4444: return 2;
        default:                     return 4;
    }
}

// Pixel -> RGBA8 empaquetado en u32 (en memoria: R, G, B, A)
u32 ToRgba(const u8* p, u32 format) {
    switch (format) {
        case PIXEL_FORMAT_RGB_565: {
            const u32 v = p[0] | (p[1] << 8);
            const u32 r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
        }
        case PIXEL_FORMAT_RGBA_4444: {
            const u32 v = p[0] | (p[1] << 8);
            const u32 r = v & 0xF, g = (v >> 4) & 0xF, b = (v >> 8) & 0xF, a = (v >> 12) & 0xF;
            return (r * 17) | ((g * 17) << 8) | ((b * 17) << 16) | ((a * 17) << 24);
        }
        case PIXEL_FORMAT_BGRA_8888:
            return p[2] | (p[1] << 8) | (p[0] << 16) | (u32(p[3]) << 24);
        case PIXEL_FORMAT_RGBX_8888:
            return p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000u;
        default: // RGBA_8888
            return p[0] | (p[1] << 8) | (p[2] << 16) | (u32(p[3]) << 24);
    }
}
} // namespace

void DeswizzleBlockLinear(const u8* in, u8* out, u32 pitch, u32 height, u32 block_height_log2) {
    const u32 block_height_gobs = 1u << block_height_log2;
    const u32 block_height_px = 8u << block_height_log2;
    const u32 width_blocks = pitch / 64;                 // un GOB mide 64 bytes de ancho
    const u32 height_blocks = (height + block_height_px - 1) / block_height_px;
    const u8* gob = in;
    for (u32 block_y = 0; block_y < height_blocks; ++block_y) {
        for (u32 block_x = 0; block_x < width_blocks; ++block_x) {
            for (u32 gob_y = 0; gob_y < block_height_gobs; ++gob_y) {
                const u32 base_y = block_y * block_height_px + gob_y * 8;
                // Un GOB son 512 bytes: 32 trozos de 16 bytes repartidos en 64x8 bytes
                for (u32 i = 0; i < 32; ++i) {
                    const u32 y = ((i >> 1) & 0x06) | (i & 0x01);
                    const u32 x = ((i << 3) & 0x10) | ((i << 1) & 0x20);
                    if (base_y + y < height)
                        std::memcpy(out + (base_y + y) * pitch + block_x * 64 + x, gob + i * 16, 16);
                }
                gob += 512;
            }
        }
    }
}

bool Display::Present(Core::Memory& memory, const GraphicBufferInfo& buf) {
    const NvMapObject* obj = m_nvmap.FindById(buf.nvmap_id);
    return obj && PresentAt(memory, buf, obj->address);
}

bool Display::PresentAt(Core::Memory& memory, const GraphicBufferInfo& buf, u64 nvmap_address) {
    if (nvmap_address == 0 || buf.width == 0 || buf.height == 0 || buf.pitch == 0) return false;
    if (buf.width > 4096 || buf.height > 4096) return false;

    // 1) Leer el buffer de la memoria del programa
    const u32 block_px = 8u << buf.block_height_log2;
    const u32 rows = (buf.layout == LAYOUT_BLOCK_LINEAR) ? ((buf.height + block_px - 1) / block_px) * block_px
                                                         : buf.height;
    std::vector<u8> raw(size_t(buf.pitch) * rows);
    memory.ReadBytes(nvmap_address + buf.offset, raw.data(), raw.size());

    // 2) Pasar a lineal si viene en bloques
    std::vector<u8> linear;
    const u8* src = raw.data();
    if (buf.layout == LAYOUT_BLOCK_LINEAR) {
        linear.assign(raw.size(), 0);
        DeswizzleBlockLinear(raw.data(), linear.data(), buf.pitch, buf.height, buf.block_height_log2);
        src = linear.data();
    } else if (buf.layout != LAYOUT_PITCH) {
        return false;
    }

    // 3) Convertir cada pixel a RGBA
    const u32 bpp = BytesPerPixel(buf.format);
    std::lock_guard lock(m_frameMutex);
    m_frame.width = buf.width;
    m_frame.height = buf.height;
    m_frame.rgba.resize(size_t(buf.width) * buf.height);
    for (u32 y = 0; y < buf.height; ++y)
        for (u32 x = 0; x < buf.width; ++x)
            m_frame.rgba[size_t(y) * buf.width + x] = ToRgba(src + size_t(y) * buf.pitch + x * bpp, buf.format);
    ++m_frame.count;
    return true;
}

} // namespace NeXo2::HLE
