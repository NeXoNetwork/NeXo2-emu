#pragma once
#include <cstdint>
#include <cstring>
#include <array>

namespace NeXo2::Core {

// Registro vectorial de 128 bits (v0..v31). Segun la instruccion se ve como:
//   q0 (128 bits), d0 (64), s0 (32), h0 (16), b0 (8)  -> siempre los bits bajos
//   v0.16b, v0.8h, v0.4s, v0.2d                      -> "carriles" (lanes) del mismo tamano
struct alignas(16) V128 {
    uint64_t lo = 0; // bits 0..63
    uint64_t hi = 0; // bits 64..127

    // Lee/escribe el carril 'index' de 'bytes' bytes (1, 2, 4 u 8)
    uint64_t Get(unsigned index, unsigned bytes) const {
        uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const uint8_t*>(this) + index * bytes, bytes);
        return value;
    }
    void Set(unsigned index, unsigned bytes, uint64_t value) {
        std::memcpy(reinterpret_cast<uint8_t*>(this) + index * bytes, &value, bytes);
    }
    bool operator==(const V128& o) const { return lo == o.lo && hi == o.hi; }
};

// Estado visible de un nucleo ARM64 (lo que un programa puede leer/escribir).
struct CPUState {
    // 31 Registros de propósito general (X0-X30). X30 = LR (direccion de retorno).
    std::array<uint64_t, 31> x;

    // 32 registros SIMD / coma flotante (v0-v31)
    std::array<V128, 32> v;

    uint64_t pc; // Program Counter (Instrucción actual)
    uint64_t sp; // Stack Pointer (Puntero de pila)

    // Banderas de estado PSTATE.NZCV:
    //   n = resultado Negativo, z = resultado Zero (cero),
    //   c = Carry (acarreo sin signo), v = oVerflow (desbordamiento con signo)
    struct {
        bool n, z, c, v;
    } flags;

    // Registros de sistema que usan los programas en EL0 (modo usuario).
    uint64_t tpidr_el0;   // puntero libre para el programa (TLS de usuario)
    uint64_t tpidrro_el0; // solo lectura para el programa: Horizon guarda aqui la TLS del hilo
    uint64_t fpcr;        // control de coma flotante (se guarda, aun no se usa)
    uint64_t fpsr;        // estado de coma flotante (se guarda, aun no se usa)

    // Inicializa todo a cero
    void Reset() {
        x.fill(0);
        v.fill(V128{});
        pc = 0;
        sp = 0;
        flags = { false, false, false, false };
        tpidr_el0 = 0;
        tpidrro_el0 = 0;
        fpcr = 0;
        fpsr = 0;
    }

    // NZCV empaquetado como en el registro real (bits 31..28).
    uint64_t GetNZCV() const {
        return (uint64_t(flags.n) << 31) | (uint64_t(flags.z) << 30) |
               (uint64_t(flags.c) << 29) | (uint64_t(flags.v) << 28);
    }
    void SetNZCV(uint64_t value) {
        flags.n = (value >> 31) & 1;
        flags.z = (value >> 30) & 1;
        flags.c = (value >> 29) & 1;
        flags.v = (value >> 28) & 1;
    }
};

} // namespace NeXo2::Core
