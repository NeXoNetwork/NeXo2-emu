#pragma once
// Utilidades compartidas por las instrucciones SIMD (NEON): leer/escribir carriles,
// saturacion, desplazamientos con redondeo y un entero de 128 bits portable.
// Solo lo incluyen interpreter_simd*.cpp y interpreter_crypto.cpp.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"
#include "fp_ops.hpp"

namespace NeXo2::Core::Simd {

using Common::Bit;
using Common::Bits;
using Common::Ones;
using Common::SignExtend;

// Mascara de un carril de 'eb' bytes
inline u64 LaneMask(unsigned eb) { return eb >= 8 ? ~0ull : ((1ull << (eb * 8)) - 1); }
// Valor con signo de un carril
inline s64 Sx(u64 v, unsigned eb) { return SignExtend(v & LaneMask(eb), eb * 8); }
inline s64 SMax(unsigned eb) { return s64(LaneMask(eb) >> 1); }
inline s64 SMin(unsigned eb) { return -SMax(eb) - 1; }

// --- Entero con signo de 128 bits (MSVC no tiene __int128) ---
struct I128 {
    u64 lo = 0;
    s64 hi = 0;
    static I128 From(s64 v) { return {u64(v), v < 0 ? -1 : 0}; }
    static I128 FromU(u64 v) { return {v, 0}; }
    I128 operator+(const I128& o) const {
        I128 r{lo + o.lo, hi + o.hi};
        if (r.lo < lo) r.hi += 1;
        return r;
    }
    I128 operator-() const {
        I128 r{~lo + 1, ~hi};
        if (r.lo == 0) r.hi += 1;
        return r;
    }
    I128 operator-(const I128& o) const { return *this + (-o); }
    I128 Shl(unsigned n) const {
        if (n == 0) return *this;
        if (n >= 128) return {};
        if (n >= 64) return {0, s64(lo << (n - 64))};
        return {lo << n, s64((u64(hi) << n) | (lo >> (64 - n)))};
    }
    I128 Sar(unsigned n) const {   // desplazamiento aritmetico a la derecha
        if (n == 0) return *this;
        if (n >= 127) return {u64(hi < 0 ? -1 : 0), hi < 0 ? -1 : 0};
        if (n >= 64) return {u64(hi >> (n - 64)), hi < 0 ? -1 : 0};
        return {(lo >> n) | (u64(hi) << (64 - n)), hi >> n};
    }
    bool operator<(const I128& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
    bool operator>(const I128& o) const { return o < *this; }
};
// Producto con signo 64x64 -> 128
inline I128 MulS(s64 a, s64 b) {
    const bool neg = (a < 0) != (b < 0);
    const u64 ua = a < 0 ? 0 - u64(a) : u64(a), ub = b < 0 ? 0 - u64(b) : u64(b);
    const u64 hi = Common::MulHighUnsigned(ua, ub), lo = ua * ub;
    I128 r{lo, s64(hi)};
    return neg ? -r : r;
}

// Satura un valor de 128 bits al rango con signo/sin signo de un carril. q = hubo saturacion.
inline u64 SatS(const I128& v, unsigned eb, bool& q) {
    const I128 mx = I128::From(SMax(eb)), mn = I128::From(SMin(eb));
    if (v > mx) { q = true; return u64(SMax(eb)) & LaneMask(eb); }
    if (v < mn) { q = true; return u64(SMin(eb)) & LaneMask(eb); }
    return v.lo & LaneMask(eb);
}
inline u64 SatU(const I128& v, unsigned eb, bool& q) {
    if (v.hi < 0) { q = true; return 0; }
    if (v.hi > 0 || (eb < 8 && v.lo > LaneMask(eb))) { q = true; return LaneMask(eb); }
    return v.lo;
}
inline u64 SatS(s64 v, unsigned eb, bool& q) { return SatS(I128::From(v), eb, q); }
inline u64 SatU(s64 v, unsigned eb, bool& q) { return SatU(I128::From(v), eb, q); }

// Valor de un carril como I128 (con o sin signo)
inline I128 Wide(u64 v, unsigned eb, bool is_unsigned) {
    return is_unsigned ? I128::FromU(v & LaneMask(eb)) : I128::From(Sx(v, eb));
}

// Desplazamiento por registro de SSHL/USHL/SRSHL/URSHL/SQSHL/UQSHL/SQRSHL/UQRSHL:
// 'shift' > 0 a la izquierda, < 0 a la derecha (con redondeo si 'round').
inline u64 ShiftByReg(u64 x, unsigned eb, s64 shift, bool is_unsigned, bool round, bool saturate, bool& q) {
    const I128 v = Wide(x, eb, is_unsigned);
    I128 r;
    if (shift >= 0) {
        // Con shift >= tamano del carril los bits se salen todos: 0, o saturar si no era 0.
        // (Hay que tratarlo aparte: con shift grande el I128 tambien se desbordaria.)
        if (shift >= s64(eb * 8) && !saturate) return 0;
        if (shift >= s64(eb * 8))
            r = (v.lo == 0 && v.hi == 0) ? I128{} : (v.hi < 0 ? I128{0, s64(1ull << 63)} : I128{~0ull, s64(~0ull >> 1)});
        else
            r = v.Shl(unsigned(shift));
        if (!saturate) return r.lo & LaneMask(eb);
    } else {
        const unsigned s = unsigned(-shift);
        r = v.Sar(s);
        if (round) r = r + I128::FromU(s > 128 ? 0 : (v.Sar(s - 1).lo & 1));
        if (!saturate) return r.lo & LaneMask(eb);
    }
    return is_unsigned ? SatU(r, eb, q) : SatS(r, eb, q);
}

// Multiplicacion polinomica (sin acarreos) de 64x64 -> 128 bits (PMULL)
inline void PolyMul64(u64 a, u64 b, u64& lo, u64& hi) {
    lo = hi = 0;
    for (unsigned i = 0; i < 64; ++i) {
        if ((b >> i) & 1) {
            lo ^= a << i;
            if (i) hi ^= a >> (64 - i);
        }
    }
}

// AdvSIMDExpandImm: el inmediato de MOVI/MVNI/ORR/BIC/FMOV (vector)
u64 ExpandImm(u32 op, u32 cmode, u32 imm8);

} // namespace NeXo2::Core::Simd
