#pragma once
#include "common/types.hpp"

// Utilidades de manipulacion de bits que usa el decodificador ARM64.
// Todas son funciones pequenas y "constexpr" para que el compilador las optimice.
namespace NeXo2::Common {

// Extrae 'count' bits empezando en el bit 'lsb'.  Ej: Bits(instr, 5, 5) = Rn.
constexpr u32 Bits(u32 value, unsigned lsb, unsigned count) {
    return (value >> lsb) & ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u));
}

// Devuelve el bit 'n' (0 o 1).
constexpr u32 Bit(u32 value, unsigned n) {
    return (value >> n) & 1u;
}

// Extiende el signo de un numero de 'bits' bits a 64 bits.
// Ej: SignExtend(0b111, 3) = -1  (los offsets de los saltos van con signo).
constexpr s64 SignExtend(u64 value, unsigned bits) {
    const u64 m = 1ULL << (bits - 1);
    value &= (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
    return static_cast<s64>((value ^ m) - m);
}

// Mascara con los 'n' bits bajos a 1 (n = 0..64).
constexpr u64 Ones(unsigned n) {
    return (n >= 64) ? ~0ULL : ((1ULL << n) - 1);
}

// Rotacion a la derecha dentro de un tamano de 'size' bits (32 o 64).
constexpr u64 RotateRight(u64 value, unsigned amount, unsigned size) {
    value &= Ones(size);
    amount %= size;
    if (amount == 0) return value;
    return ((value >> amount) | (value << (size - amount))) & Ones(size);
}

// Copia un patron de 'esize' bits hasta llenar 'size' bits.
constexpr u64 Replicate(u64 pattern, unsigned esize, unsigned size) {
    u64 result = 0;
    for (unsigned i = 0; i < size; i += esize) result |= pattern << i;
    return result & Ones(size);
}

// Parte alta (bits 64..127) de multiplicar dos u64 sin signo.
// Se hace "a mano" con mitades de 32 bits porque MSVC no tiene __int128.
constexpr u64 MulHighUnsigned(u64 a, u64 b) {
    const u64 a_lo = a & 0xFFFFFFFFu, a_hi = a >> 32;
    const u64 b_lo = b & 0xFFFFFFFFu, b_hi = b >> 32;
    const u64 lo_lo = a_lo * b_lo;
    const u64 hi_lo = a_hi * b_lo;
    const u64 lo_hi = a_lo * b_hi;
    const u64 hi_hi = a_hi * b_hi;
    const u64 cross = (lo_lo >> 32) + (hi_lo & 0xFFFFFFFFu) + lo_hi;
    return hi_hi + (hi_lo >> 32) + (cross >> 32);
}

// Parte alta de multiplicar dos s64 con signo (a partir de la version sin signo).
constexpr u64 MulHighSigned(s64 a, s64 b) {
    u64 high = MulHighUnsigned(static_cast<u64>(a), static_cast<u64>(b));
    if (a < 0) high -= static_cast<u64>(b);
    if (b < 0) high -= static_cast<u64>(a);
    return high;
}

// "DecodeBitMasks" del manual de ARM (pseudocodigo de la arquitectura).
// Convierte los campos N:immr:imms de las instrucciones logicas/bitfield en
// las mascaras reales. Devuelve false si la combinacion es invalida.
inline bool DecodeBitMasks(unsigned n, unsigned imms, unsigned immr, bool immediate,
                           unsigned datasize, u64& wmask, u64& tmask) {
    // len = posicion del bit mas alto de N:NOT(imms)
    const unsigned combined = (n << 6) | (~imms & 0x3F);
    int len = -1;
    for (int i = 6; i >= 0; --i) {
        if (combined & (1u << i)) { len = i; break; }
    }
    if (len < 1) return false;

    const unsigned esize  = 1u << len;
    const unsigned levels = esize - 1;
    if (immediate && (imms & levels) == levels) return false;

    const unsigned s    = imms & levels;
    const unsigned r    = immr & levels;
    const unsigned diff = (s - r) & levels;

    const u64 welem = Ones(s + 1);
    const u64 telem = Ones(diff + 1);
    wmask = Replicate(RotateRight(welem, r, esize), esize, datasize);
    tmask = Replicate(telem, esize, datasize);
    return true;
}

} // namespace NeXo2::Common
