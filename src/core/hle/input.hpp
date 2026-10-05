#pragma once
#include <vector>
#include "common/types.hpp"
#include "memory.hpp"

// Mandos emulados.
//
// En la Switch el estado de los mandos no se pide por IPC: el servicio hid escribe
// continuamente en una memoria compartida de 0x40000 bytes y el programa la lee
// (libnx: padUpdate -> hidGetNpadStates*). Cada mando tiene "colas" (LIFO) de 17
// estados; el mas reciente esta en la posicion 'tail'.
//
// NeXo conecta dos mandos con los mismos botones:
//   - No1      como mando Pro       (estilo FullKey)
//   - Handheld como Joy-Con en la consola (estilo Handheld)
// La interfaz (main.cpp) llama a Kernel::SetPadInput() con el teclado / mando del PC.
// Formato: docs/07-nexo-internals/input.md
namespace NeXo2::HLE {

// Botones (HidNpadButton de libnx)
namespace NpadButton {
    constexpr u64 A = 1ull << 0,  B = 1ull << 1,  X = 1ull << 2,  Y = 1ull << 3;
    constexpr u64 StickL = 1ull << 4, StickR = 1ull << 5;
    constexpr u64 L = 1ull << 6,  R = 1ull << 7,  ZL = 1ull << 8, ZR = 1ull << 9;
    constexpr u64 Plus = 1ull << 10, Minus = 1ull << 11;
    constexpr u64 Left = 1ull << 12, Up = 1ull << 13, Right = 1ull << 14, Down = 1ull << 15;
    // "Botones" virtuales que hid activa cuando un stick se inclina mucho
    constexpr u64 StickLLeft = 1ull << 16, StickLUp = 1ull << 17, StickLRight = 1ull << 18, StickLDown = 1ull << 19;
    constexpr u64 StickRLeft = 1ull << 20, StickRUp = 1ull << 21, StickRRight = 1ull << 22, StickRDown = 1ull << 23;
}

constexpr s32 STICK_MAX = 0x7FFF;

// Estado de un mando en un instante. Sticks de -STICK_MAX a STICK_MAX (arriba = +y).
struct PadInput {
    u64 buttons = 0;
    s32 lx = 0, ly = 0;
    s32 rx = 0, ry = 0;
};

// Offsets dentro de la memoria compartida de hid (comprobados con las cabeceras de libnx)
namespace HidLayout {
    constexpr size_t SIZE            = 0x40000;
    constexpr size_t NPAD            = 0x9A00;  // 10 mandos de 0x5000 bytes
    constexpr size_t NPAD_ENTRY_SIZE = 0x5000;
    constexpr size_t NPAD_HANDHELD   = 8;       // indice del mando "Handheld"
    // Dentro de cada mando (HidNpadInternalState)
    constexpr size_t STYLE_SET       = 0x00;
    constexpr size_t FULL_KEY_COLOR  = 0x08;
    constexpr size_t JOY_COLOR       = 0x14;
    constexpr size_t FULL_KEY_LIFO   = 0x28;
    constexpr size_t HANDHELD_LIFO   = 0x378;
    constexpr size_t DEVICE_TYPE     = 0x4188;
    // Cola (HidNpadCommonLifo): cabecera de 0x20 + 17 entradas de 0x30
    constexpr size_t LIFO_SIZE       = 0x350;
    constexpr size_t LIFO_ENTRIES    = 17;
    constexpr size_t LIFO_STORAGE    = 0x20;
    constexpr size_t LIFO_ENTRY_SIZE = 0x30;
}

class InputState {
public:
    // Deja los dos mandos "conectados" (sin botones pulsados) en una memoria nueva
    void InitSharedMemory(std::vector<u8>& shm);

    // Anade un estado nuevo a las colas. Escribe en 'shm' y, si el programa ya
    // mapeo la memoria (mapped_address != 0), tambien en su copia.
    void Update(std::vector<u8>& shm, Core::Memory& memory, u64 mapped_address, const PadInput& pad);

    void Reset() { m_sampling = 0; }
    u64 SamplingNumber() const { return m_sampling; }

    // Anade a 'buttons' los botones virtuales de los sticks
    static u64 StickButtons(const PadInput& pad);

private:
    void PushState(std::vector<u8>& shm, size_t lifo_offset, const PadInput& pad, u32 attributes);
    u64 m_sampling = 0;
};

} // namespace NeXo2::HLE
