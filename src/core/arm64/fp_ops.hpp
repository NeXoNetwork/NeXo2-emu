#pragma once
#include "common/types.hpp"
#include <cstring>

// Coma flotante con las reglas EXACTAS de ARM (pseudocodigo del manual, capitulo
// "Floating-point" / shared/functions/float). Trabaja con los bits de los numeros
// (u64) en los tres formatos: 16 (half), 32 (single) y 64 (double).
//
// Por que no basta con la coma flotante del PC:
//   - NaN: ARM devuelve el primer sNaN "silenciado" conservando signo y contenido;
//     el PC (x86) da otro NaN. Y el NaN "por defecto" de ARM es positivo; el de x86, negativo.
//   - FPCR: modos de redondeo, FZ (los numeros subnormales se vuelven 0), DN (NaN por
//     defecto siempre), AHP (formato half alternativo) y FZ16.
//   - FPSR: los flags de excepcion (operacion invalida, division por cero, etc.).
//   - Estimaciones FRECPE/FRSQRTE: tablas propias de ARM.
//   - Half (16 bits): el PC no tiene aritmetica de 16 bits.
//
// Como se hace: las operaciones basicas (+ - * / sqrt fma) de 32 y 64 bits usan la
// FPU del PC con su modo de redondeo puesto igual que FPCR, y antes/despues se
// aplican las reglas de ARM (NaN, FZ...). Todo lo demas (redondear a otro formato,
// conversiones con enteros, FRINT, half...) se hace en software, bit a bit.
//
// Flags de FPSR: los que calcula el software se guardan en 'fpsr' al momento; los de
// la FPU del PC se recogen con FoldHostFlags() cuando el programa lee FPSR.
namespace NeXo2::Core::FP {

// FPCR
constexpr u32 FPCR_AHP  = 1u << 26;
constexpr u32 FPCR_DN   = 1u << 25;
constexpr u32 FPCR_FZ   = 1u << 24;
constexpr u32 FPCR_FZ16 = 1u << 19;
inline u32 RoundingMode(u32 fpcr) { return (fpcr >> 22) & 3; }   // 0 RN, 1 RP, 2 RM, 3 RZ

// FPSR
constexpr u64 FPSR_IOC = 1u << 0;   // operacion invalida
constexpr u64 FPSR_DZC = 1u << 1;   // division por cero
constexpr u64 FPSR_OFC = 1u << 2;   // desbordamiento
constexpr u64 FPSR_UFC = 1u << 3;   // subdesbordamiento
constexpr u64 FPSR_IXC = 1u << 4;   // resultado inexacto
constexpr u64 FPSR_IDC = 1u << 7;   // entrada subnormal (solo con FZ)
constexpr u64 FPSR_QC  = 1u << 27;  // saturacion (instrucciones enteras SQADD...)

// Modos de redondeo (los de FPCR y los que fuerzan algunas instrucciones)
enum Rounding : u32 { RN = 0, RP = 1, RM = 2, RZ = 3, RA = 4 /* al mas cercano, empates lejos del 0 */, RO = 5 /* a impar */ };

struct Env {
    u32  fpcr;
    u64& fpsr;
};

// --- Informacion de los formatos ---
enum class Type { Zero, Denormal, Normal, Infinity, QNaN, SNaN };
Type Classify(u64 bits, unsigned w);
bool IsNaN(u64 bits, unsigned w);
u64  DefaultNaN(unsigned w);
u64  Zero(bool sign, unsigned w);
u64  Infinity(bool sign, unsigned w);
u64  MaxNormal(bool sign, unsigned w);
inline u64 SignBit(unsigned w) { return 1ull << (w - 1); }
u64  Abs(u64 a, unsigned w);
u64  Neg(u64 a, unsigned w);

// --- Aritmetica ---
u64 Add(u64 a, u64 b, unsigned w, Env& e);
u64 Sub(u64 a, u64 b, unsigned w, Env& e);
u64 Mul(u64 a, u64 b, unsigned w, Env& e);
u64 MulX(u64 a, u64 b, unsigned w, Env& e);          // FMULX: inf * 0 = +-2.0
u64 Div(u64 a, u64 b, unsigned w, Env& e);
u64 MulAdd(u64 addend, u64 a, u64 b, unsigned w, Env& e);   // addend + a*b con un solo redondeo
u64 Sqrt(u64 a, unsigned w, Env& e);
u64 Max(u64 a, u64 b, unsigned w, Env& e);
u64 Min(u64 a, u64 b, unsigned w, Env& e);
u64 MaxNum(u64 a, u64 b, unsigned w, Env& e);        // FMAXNM: un qNaN pierde contra un numero
u64 MinNum(u64 a, u64 b, unsigned w, Env& e);
u64 RecipEstimate(u64 a, unsigned w, Env& e);        // FRECPE
u64 RSqrtEstimate(u64 a, unsigned w, Env& e);        // FRSQRTE
u64 RecipStepFused(u64 a, u64 b, unsigned w, Env& e);   // FRECPS: 2 - a*b
u64 RSqrtStepFused(u64 a, u64 b, unsigned w, Env& e);   // FRSQRTS: (3 - a*b) / 2
u64 RecpX(u64 a, unsigned w, Env& e);                // FRECPX
u32 URecipEstimate(u32 a);                           // URECPE
u32 URSqrtEstimate(u32 a);                           // URSQRTE

// --- Comparaciones ---
u32  Compare(u64 a, u64 b, unsigned w, bool signal_qnan, Env& e);   // devuelve NZCV (bits 3..0)
bool CompareEQ(u64 a, u64 b, unsigned w, Env& e);
bool CompareGE(u64 a, u64 b, unsigned w, Env& e);
bool CompareGT(u64 a, u64 b, unsigned w, Env& e);

// --- Redondeo y conversiones ---
u64 RoundInt(u64 a, unsigned w, Rounding r, bool exact, Env& e);    // FRINT*
// Coma flotante -> entero (o coma fija con 'fbits' bits de fraccion), con saturacion
u64 ToFixed(u64 a, unsigned w, unsigned fbits, bool is_unsigned, Rounding r, unsigned out_bits, Env& e);
// Entero (o coma fija) -> coma flotante
u64 FixedToFP(u64 v, unsigned in_bits, unsigned fbits, bool is_unsigned, unsigned w, Rounding r, Env& e);
// Entre formatos (FCVT). 'r' normalmente el de FPCR; FCVTXN usa RO (a impar).
u64 Convert(u64 a, unsigned from_w, unsigned to_w, Rounding r, Env& e);

// --- Camino rapido (inline, para el bucle del interprete) ---
// Con FPCR "normal" (redondeo al mas cercano, sin FZ ni DN) y sin NaN en la entrada,
// la FPU del PC da en 32 y 64 bits exactamente el mismo resultado que ARM (los dos
// siguen IEEE 754) y los mismos flags. Solo cambia el NaN que se genera (0/0, inf-inf...):
// el de ARM es positivo. Devuelven false si hay que ir por el camino completo.
namespace Fast {
inline bool FpcrOk(u32 fpcr) { return (fpcr & (FPCR_FZ | FPCR_DN | (3u << 22))) == 0; }
inline bool NaN32(u64 a) { return u32(a << 1) > 0xFF000000u; }
inline bool NaN64(u64 a) { return (a << 1) > 0xFFE0000000000000ull; }
inline float  F32(u64 b) { float f; const u32 x = u32(b); std::memcpy(&f, &x, 4); return f; }
inline double F64(u64 b) { double f; std::memcpy(&f, &b, 8); return f; }
inline u64 Out32(float r)  { u32 x; std::memcpy(&x, &r, 4); return r == r ? x : 0x7FC00000u; }
inline u64 Out64(double r) { u64 x; std::memcpy(&x, &r, 8); return r == r ? x : 0x7FF8000000000000ull; }

enum Op : u32 { ADD, SUB, MUL, DIV };
template <Op OP, typename T> inline T Apply(T x, T y) {
    if constexpr (OP == ADD) return x + y;
    else if constexpr (OP == SUB) return x - y;
    else if constexpr (OP == MUL) return x * y;
    else return x / y;
}
template <Op OP> inline bool Arith(u64 a, u64 b, unsigned w, u32 fpcr, u64& out) {
    if (!FpcrOk(fpcr)) return false;
    if (w == 32) {
        if (NaN32(a) || NaN32(b)) return false;
        out = Out32(Apply<OP>(F32(a), F32(b)));
        return true;
    }
    if (w == 64) {
        if (NaN64(a) || NaN64(b)) return false;
        out = Out64(Apply<OP>(F64(a), F64(b)));
        return true;
    }
    return false;
}
} // namespace Fast

// Flags de la FPU del PC -> FPSR, y borrarlos. Llamar al leer FPSR o al cambiar de hilo.
void FoldHostFlags(u64& fpsr);
void ClearHostFlags();

} // namespace NeXo2::Core::FP
