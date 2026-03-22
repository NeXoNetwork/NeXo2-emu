#pragma once
#include <cstdint>
#include <array>

namespace NeXo2::Core {

struct CPUState {
    // 31 Registros de propósito general (X0-X30)
    std::array<uint64_t, 31> x;

    uint64_t pc; // Program Counter (Instrucción actual)
    uint64_t sp; // Stack Pointer (Puntero de pila)

    // Banderas de estado (Zero, Negative, Carry, Overflow)
    struct {
        bool n, z, c, v;
    } flags;

    // Inicializa todo a cero
    void Reset() {
        x.fill(0);
        pc = 0;
        sp = 0;
        flags = { false, false, false, false };
    }
};

} // namespace NeXo2::Core