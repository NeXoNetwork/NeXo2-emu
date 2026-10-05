#include "input.hpp"
#include <algorithm>
#include <cstring>

namespace NeXo2::HLE {
using namespace HidLayout;

namespace {
constexpr u32 STYLE_FULL_KEY = 1u << 0;   // mando Pro
constexpr u32 STYLE_HANDHELD = 1u << 1;   // Joy-Con en la consola
constexpr u32 ATTR_CONNECTED = 1u << 0;
constexpr u32 ATTR_WIRED     = 1u << 1;
constexpr u32 ATTR_LEFT_RIGHT_CONNECTED_WIRED = 0x3C;  // Joy-Con izquierdo y derecho, conectados y por cable
constexpr u32 DEVICE_FULL_KEY = 1u << 0;
constexpr u32 DEVICE_HANDHELD = (1u << 2) | (1u << 3);

template <typename T> void Put(std::vector<u8>& shm, size_t offset, T value) {
    std::memcpy(shm.data() + offset, &value, sizeof(T));
}
template <typename T> T Get(const std::vector<u8>& shm, size_t offset) {
    T v; std::memcpy(&v, shm.data() + offset, sizeof(T)); return v;
}

size_t NpadBase(size_t index) { return NPAD + index * NPAD_ENTRY_SIZE; }

// Colores del mando (los menus del sistema los usan para dibujarlo)
void PutColors(std::vector<u8>& shm, size_t npad) {
    Put<u32>(shm, npad + FULL_KEY_COLOR + 0, 0);           // atributo: colores validos
    Put<u32>(shm, npad + FULL_KEY_COLOR + 4, 0xFF2D2D2D);  // cuerpo gris oscuro
    Put<u32>(shm, npad + FULL_KEY_COLOR + 8, 0xFFE6E6E6);  // botones
    Put<u32>(shm, npad + JOY_COLOR + 0, 0);
    Put<u32>(shm, npad + JOY_COLOR + 4,  0xFFEB4600);      // Joy-Con izquierdo
    Put<u32>(shm, npad + JOY_COLOR + 8,  0xFF1E1E1E);
    Put<u32>(shm, npad + JOY_COLOR + 12, 0xFFC80A3C);      // Joy-Con derecho
    Put<u32>(shm, npad + JOY_COLOR + 16, 0xFF1E1E1E);
}
} // namespace

u64 InputState::StickButtons(const PadInput& pad) {
    constexpr s32 T = STICK_MAX / 2;   // a partir de media inclinacion cuenta como "pulsado"
    u64 b = 0;
    if (pad.lx < -T) b |= NpadButton::StickLLeft;
    if (pad.lx >  T) b |= NpadButton::StickLRight;
    if (pad.ly >  T) b |= NpadButton::StickLUp;
    if (pad.ly < -T) b |= NpadButton::StickLDown;
    if (pad.rx < -T) b |= NpadButton::StickRLeft;
    if (pad.rx >  T) b |= NpadButton::StickRRight;
    if (pad.ry >  T) b |= NpadButton::StickRUp;
    if (pad.ry < -T) b |= NpadButton::StickRDown;
    return b;
}

void InputState::PushState(std::vector<u8>& shm, size_t lifo, const PadInput& pad, u32 attributes) {
    // Cabecera: u64 sin uso, u64 tamano (17), u64 tail (ultima escrita), u64 count
    const u64 tail  = (Get<u64>(shm, lifo + 0x10) + 1) % LIFO_ENTRIES;
    const u64 count = std::min<u64>(Get<u64>(shm, lifo + 0x18) + 1, LIFO_ENTRIES);

    // Entrada: u64 sampling_number + HidNpadCommonState
    //   +0x00 sampling_number, +0x08 buttons, +0x10 stick L (x, y), +0x18 stick R, +0x20 atributos
    const size_t e = lifo + LIFO_STORAGE + tail * LIFO_ENTRY_SIZE;
    Put<u64>(shm, e + 0x00, m_sampling);
    Put<u64>(shm, e + 0x08, m_sampling);
    Put<u64>(shm, e + 0x10, pad.buttons | StickButtons(pad));
    Put<s32>(shm, e + 0x18, pad.lx);
    Put<s32>(shm, e + 0x1C, pad.ly);
    Put<s32>(shm, e + 0x20, pad.rx);
    Put<s32>(shm, e + 0x24, pad.ry);
    Put<u32>(shm, e + 0x28, attributes);
    Put<u32>(shm, e + 0x2C, 0);

    Put<u64>(shm, lifo + 0x08, LIFO_ENTRIES);
    Put<u64>(shm, lifo + 0x10, tail);
    Put<u64>(shm, lifo + 0x18, count);
}

void InputState::InitSharedMemory(std::vector<u8>& shm) {
    shm.assign(SIZE, 0);
    m_sampling = 0;
    const size_t no1 = NpadBase(0), handheld = NpadBase(NPAD_HANDHELD);
    Put<u32>(shm, no1 + STYLE_SET, STYLE_FULL_KEY);
    Put<u32>(shm, no1 + DEVICE_TYPE, DEVICE_FULL_KEY);
    Put<u32>(shm, handheld + STYLE_SET, STYLE_HANDHELD);
    Put<u32>(shm, handheld + DEVICE_TYPE, DEVICE_HANDHELD);
    PutColors(shm, no1);
    PutColors(shm, handheld);
    // Un primer estado (nada pulsado) para que el mando aparezca conectado desde el principio
    PushState(shm, no1 + FULL_KEY_LIFO, PadInput{}, ATTR_CONNECTED);
    PushState(shm, handheld + HANDHELD_LIFO, PadInput{}, ATTR_CONNECTED | ATTR_WIRED | ATTR_LEFT_RIGHT_CONNECTED_WIRED);
    ++m_sampling;
}

void InputState::Update(std::vector<u8>& shm, Core::Memory& memory, u64 mapped_address, const PadInput& pad) {
    if (shm.size() < SIZE) return;
    const size_t no1 = NpadBase(0), handheld = NpadBase(NPAD_HANDHELD);
    PushState(shm, no1 + FULL_KEY_LIFO, pad, ATTR_CONNECTED);
    PushState(shm, handheld + HANDHELD_LIFO, pad, ATTR_CONNECTED | ATTR_WIRED | ATTR_LEFT_RIGHT_CONNECTED_WIRED);
    ++m_sampling;

    // Copiar solo las dos colas a la memoria del programa
    if (mapped_address != 0) {
        memory.WriteBytes(mapped_address + no1 + FULL_KEY_LIFO, shm.data() + no1 + FULL_KEY_LIFO, LIFO_SIZE);
        memory.WriteBytes(mapped_address + handheld + HANDHELD_LIFO, shm.data() + handheld + HANDHELD_LIFO, LIFO_SIZE);
    }
}

} // namespace NeXo2::HLE
