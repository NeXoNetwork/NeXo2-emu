// Tests de la pantalla emulada (vi + nvdrv) y de la tarjeta SD (fsp-srv).
#include <cstring>
#include <vector>

#include "test_framework.hpp"
#include "hle/display.hpp"
#include "hle/services/fs.hpp"
#include "hle/services/vi.hpp"

using namespace NeXo2;
using namespace NeXo2::HLE;

namespace {
// Direccion de un byte (x en bytes, y en filas) dentro de una imagen "block linear".
// Es la formula clasica de las GPUs Tegra, escrita de forma independiente a
// DeswizzleBlockLinear para que el test compruebe algo de verdad.
size_t BlockLinearOffset(u32 x, u32 y, u32 pitch, u32 block_height_log2) {
    const u32 gobs_per_block = 1u << block_height_log2;
    const u32 block_rows = 8 * gobs_per_block;
    const size_t block_size = 512ull * gobs_per_block;
    const size_t blocks_per_row = pitch / 64;
    size_t off = (y / block_rows) * blocks_per_row * block_size;   // fila de bloques
    off += (x / 64) * block_size;                                   // bloque dentro de la fila
    off += ((y % block_rows) / 8) * 512;                            // GOB dentro del bloque
    // Dentro del GOB (64x8 bytes)
    off += ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
    return off;
}
} // namespace

TEST(Display_DeswizzleBlockLinear) {
    const u32 pitch = 256, height = 40, bhl2 = 2;   // 40 filas -> 2 bloques de 32 (el ultimo a medias)
    std::vector<u8> linear(pitch * 64), swizzled(pitch * 64, 0), out(pitch * 64, 0);
    for (size_t i = 0; i < linear.size(); ++i) linear[i] = u8(i * 7 + (i >> 8));
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < pitch; ++x)
            swizzled[BlockLinearOffset(x, y, pitch, bhl2)] = linear[y * pitch + x];

    DeswizzleBlockLinear(swizzled.data(), out.data(), pitch, height, bhl2);
    bool same = true;
    for (u32 y = 0; y < height; ++y)
        if (std::memcmp(&out[y * pitch], &linear[y * pitch], pitch) != 0) same = false;
    CHECK(same);
}

TEST(Display_PresentRgb565Pitch) {
    Core::Memory mem;
    const u64 base = 0x80000000;
    mem.MapRegion(base, 0x1000, Core::MemoryState::Normal, Core::MemoryPermission::ReadWrite, "fb");
    // Imagen de 4x2 en RGB565, pitch 16 bytes: rojo, verde, azul, blanco...
    const u16 px[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xF800, 0xF800, 0xF800};
    mem.WriteBytes(base, px, 8);
    mem.WriteBytes(base + 16, px + 4, 8);

    Display display;
    auto& obj = display.NvMap().Create(0x1000);
    obj.address = base;
    GraphicBufferInfo info;
    info.nvmap_id = obj.id; info.width = 4; info.height = 2; info.format = 4;
    info.layout = 1; info.pitch = 16;
    CHECK(display.Present(mem, info));
    const auto& f = display.Frame();
    CHECK_EQ(f.count, 1);
    CHECK_EQ(f.rgba[0], 0xFF0000FFu);   // rojo  (en memoria: R G B A)
    CHECK_EQ(f.rgba[1], 0xFF00FF00u);   // verde
    CHECK_EQ(f.rgba[2], 0xFFFF0000u);   // azul
    CHECK_EQ(f.rgba[3], 0xFFFFFFFFu);   // blanco
    CHECK_EQ(f.rgba[4], 0xFF000000u);   // negro (fila 2, despues del pitch)

    // Un id de nvmap que no existe no se presenta
    info.nvmap_id = 99;
    CHECK(!display.Present(mem, info));
}

TEST(Display_ParseGraphicBuffer) {
    // GraphicBuffer real que manda libnx (consola 1280x720 RGB565 en block linear)
    const u32 words[] = {
        0x47424652, 0x500, 0x2D0, 0x500, 4, 0xB00, 0x2A, 0, 0, 0x51,
        0xFFFFFFFF, 1, 0, 0xDAFFCAFF, 0x2A, 0, 0xB00, 4, 4, 0x500, 0x1E0000, 1, 0,
        0x500, 0x2D0, 0x0A881210, 1, 3, 0xA00, 0, 0, 0xFE, 4, 0, 0, 0, 0, 0x1E0000, 0, 0};
    std::vector<u8> flat(sizeof(words));
    std::memcpy(flat.data(), words, sizeof(words));
    GraphicBufferInfo info;
    CHECK(ParseGraphicBuffer(flat, info));
    CHECK_EQ(info.nvmap_id, 1);
    CHECK_EQ(info.width, 1280);
    CHECK_EQ(info.height, 720);
    CHECK_EQ(info.format, 4);
    CHECK_EQ(info.layout, 3);
    CHECK_EQ(info.pitch, 0xA00);
    CHECK_EQ(info.block_height_log2, 4);
    CHECK_EQ(info.size, 0x1E0000);
    flat[0] = 0;   // sin "GBFR" no es un GraphicBuffer
    CHECK(!ParseGraphicBuffer(flat, info));
}

TEST(Fs_ResolveGuestPath) {
    const std::filesystem::path root = "sd";
    CHECK(ResolveGuestPath(root, "/config/app.ini") == root / "config" / "app.ini");
    CHECK(ResolveGuestPath(root, "//a/./b") == root / "a" / "b");
    CHECK(ResolveGuestPath(root, "/") == root);
    // Nunca se puede salir de la carpeta de la SD
    CHECK(ResolveGuestPath(root, "/../secreto.txt").empty());
    CHECK(ResolveGuestPath(root, "/a/../../b").empty());
    CHECK(ResolveGuestPath(root, "/a\\..\\b").empty());
    CHECK(ResolveGuestPath(root, "/C:/Windows").empty());
}
