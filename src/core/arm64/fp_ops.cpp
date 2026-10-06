// Coma flotante con las reglas de ARM. Ver fp_ops.hpp.
//
// Cada funcion sigue el pseudocodigo del manual de ARM (FPAdd, FPMulAdd, FPRound,
// FPToFixed, FPRecipEstimate...). Los nombres de las variables siguen los del manual
// para poder compararlos.
//
// IMPORTANTE: este archivo se compila con -frounding-math (GCC/Clang) o /fp:strict
// (MSVC) para que el compilador respete el modo de redondeo que ponemos en la FPU.
#include "fp_ops.hpp"

#include <initializer_list>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <utility>

namespace NeXo2::Core::FP {

namespace {

struct Fmt { unsigned w, ebits, fbits; int bias; };
constexpr Fmt FmtOf(unsigned w) {
    return w == 16 ? Fmt{16, 5, 10, 15} : w == 32 ? Fmt{32, 8, 23, 127} : Fmt{64, 11, 52, 1023};
}
inline u64 Mask(unsigned n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }
inline unsigned Clz64(u64 v) {
    unsigned n = 0;
    if (v == 0) return 64;
    while (!(v & (1ull << 63))) { v <<= 1; ++n; }
    return n;
}

inline bool SignOf(u64 b, unsigned w) { return (b >> (w - 1)) & 1; }
inline u64  ExpField(u64 b, const Fmt& f) { return (b >> f.fbits) & Mask(f.ebits); }
inline u64  FracField(u64 b, const Fmt& f) { return b & Mask(f.fbits); }
inline u64  Pack(bool s, u64 e, u64 frac, const Fmt& f) { return (u64(s) << (f.w - 1)) | (e << f.fbits) | frac; }

inline float  F32(u64 b) { float f; u32 x = u32(b); std::memcpy(&f, &x, 4); return f; }
inline double F64(u64 b) { double d; std::memcpy(&d, &b, 8); return d; }
inline u64 B32(float f)  { u32 x; std::memcpy(&x, &f, 4); return x; }
inline u64 B64(double d) { u64 x; std::memcpy(&x, &d, 8); return x; }

// ¿Se aplica "flush to zero" a este formato? (FZ para 32/64, FZ16 para 16)
inline bool FlushOn(unsigned w, u32 fpcr) {
    return w == 16 ? (fpcr & FPCR_FZ16) != 0 : (fpcr & FPCR_FZ) != 0;
}

// FPUnpack: tipo + signo + valor exacto (mant * 2^exp). Con FZ los subnormales son 0.
struct Unpacked { Type type; bool sign; int exp; u64 mant; };
Unpacked Unpack(u64 b, unsigned w, Env& e, bool allow_fz = true) {
    const Fmt f = FmtOf(w);
    Unpacked u{Classify(b, w), SignOf(b, w), 0, 0};
    const u64 ef = ExpField(b, f), frac = FracField(b, f);
    if (u.type == Type::Denormal) {
        if (allow_fz && FlushOn(w, e.fpcr)) {
            u.type = Type::Zero;
            if (w != 16) e.fpsr |= FPSR_IDC;      // en half no se marca IDC
        } else {
            u.mant = frac;
            u.exp = 1 - f.bias - int(f.fbits);
        }
    } else if (u.type == Type::Normal) {
        u.mant = frac | (1ull << f.fbits);
        u.exp = int(ef) - f.bias - int(f.fbits);
    }
    return u;
}

// Bits de la entrada tras aplicar FZ (para pasarla a la FPU del PC)
inline bool IsNaNType(Type t) { return t == Type::QNaN || t == Type::SNaN; }

u64 FlushInput(u64 b, unsigned w, Env& e) {
    if (Classify(b, w) == Type::Denormal && FlushOn(w, e.fpcr)) {
        if (w != 16) e.fpsr |= FPSR_IDC;
        return Zero(SignOf(b, w), w);
    }
    return b;
}

// FPProcessNaN: silenciar un sNaN (y marcar IOC), o el NaN por defecto si FPCR.DN
u64 ProcessNaN(u64 b, unsigned w, Env& e) {
    const Fmt f = FmtOf(w);
    if (Classify(b, w) == Type::SNaN) {
        e.fpsr |= FPSR_IOC;
        b |= 1ull << (f.fbits - 1);
    }
    return (e.fpcr & FPCR_DN) ? DefaultNaN(w) : b;
}

// FPProcessNaNs: primero cualquier sNaN (en orden), despues cualquier qNaN
// ARM "desempaqueta" (FPUnpack) todos los operandos antes de mirar los NaN: un
// subnormal con FZ marca IDC aunque el resultado acabe siendo un NaN.
void NoteDenormals(std::initializer_list<u64> ops, unsigned w, Env& e) {
    for (u64 x : ops) (void)FlushInput(x, w, e);
}

bool ProcessNaNs(u64 a, u64 b, unsigned w, Env& e, u64& out) {
    const Type ta = Classify(a, w), tb = Classify(b, w);
    if (IsNaNType(ta) || IsNaNType(tb)) NoteDenormals({a, b}, w, e);
    if (ta == Type::SNaN) { out = ProcessNaN(a, w, e); return true; }
    if (tb == Type::SNaN) { out = ProcessNaN(b, w, e); return true; }
    if (ta == Type::QNaN) { out = ProcessNaN(a, w, e); return true; }
    if (tb == Type::QNaN) { out = ProcessNaN(b, w, e); return true; }
    return false;
}
bool ProcessNaNs3(u64 a, u64 b, u64 c, unsigned w, Env& e, u64& out) {
    const Type ta = Classify(a, w), tb = Classify(b, w), tc = Classify(c, w);
    if (IsNaNType(ta) || IsNaNType(tb) || IsNaNType(tc)) NoteDenormals({a, b, c}, w, e);
    if (ta == Type::SNaN) { out = ProcessNaN(a, w, e); return true; }
    if (tb == Type::SNaN) { out = ProcessNaN(b, w, e); return true; }
    if (tc == Type::SNaN) { out = ProcessNaN(c, w, e); return true; }
    if (ta == Type::QNaN) { out = ProcessNaN(a, w, e); return true; }
    if (tb == Type::QNaN) { out = ProcessNaN(b, w, e); return true; }
    if (tc == Type::QNaN) { out = ProcessNaN(c, w, e); return true; }
    return false;
}

// FPRound: valor exacto (-1)^sign * mant * 2^exp (+ 'sticky' = habia mas bits por debajo)
// -> bits del formato 'w', con el modo de redondeo r. Marca IXC/UFC/OFC.
u64 RoundTo(bool sign, int exp, u64 mant, bool sticky, unsigned w, Rounding r, Env& e,
            bool use_fz, bool ahp = false) {
    const Fmt f = FmtOf(w);
    if (mant == 0) return Zero(sign, w);
    const unsigned lz = Clz64(mant);
    mant <<= lz;
    exp -= int(lz);
    int E = exp + 63;                         // el valor esta en [2^E, 2^(E+1))
    const int emin = 1 - f.bias;

    if (E < emin && use_fz && FlushOn(w, e.fpcr)) {
        e.fpsr |= FPSR_UFC;                   // con FZ: a 0, solo UFC
        return Zero(sign, w);
    }

    // Bits que se descartan: los que no caben en fbits+1 (mas los de subnormal)
    int drop = 63 - int(f.fbits);
    const bool tiny = E < emin;
    if (tiny) drop += emin - E;

    u64 kept;
    bool round_bit, sticky_bits;
    if (drop >= 64) {
        kept = 0;
        round_bit = (drop == 64) && ((mant >> 63) & 1);
        sticky_bits = sticky || (drop == 64 ? (mant << 1) != 0 : mant != 0);
    } else {
        kept = mant >> drop;
        const u64 rem = mant & Mask(unsigned(drop));
        round_bit = (rem >> (drop - 1)) & 1;
        sticky_bits = sticky || (rem & Mask(unsigned(drop - 1))) != 0;
    }
    const bool inexact = round_bit || sticky_bits;
    bool up = false;
    switch (r) {
        case RN: up = round_bit && (sticky_bits || (kept & 1)); break;
        case RA: up = round_bit; break;
        case RP: up = inexact && !sign; break;
        case RM: up = inexact && sign; break;
        case RO: if (inexact) kept |= 1; break;
        default: break;
    }
    kept += up ? 1 : 0;

    u64 biased, frac;
    if (tiny) {
        biased = (kept >> f.fbits) ? 1 : 0;   // puede haber subido al menor normal
        frac = kept & Mask(f.fbits);
    } else {
        if (kept >> (f.fbits + 1)) { kept >>= 1; ++E; }
        biased = u64(E + f.bias);
        frac = kept & Mask(f.fbits);
    }

    const u64 max_exp = Mask(f.ebits);
    if (ahp) {                                // half alternativo: sin infinitos ni NaN
        if (!tiny && biased > max_exp) {
            e.fpsr |= FPSR_IOC;
            return (u64(sign) << 15) | 0x7FFF;
        }
    } else if (!tiny && biased >= max_exp) {  // desbordamiento
        e.fpsr |= FPSR_OFC | FPSR_IXC;
        const bool to_inf = (r == RN || r == RA) || (r == RP && !sign) || (r == RM && sign);
        return to_inf ? Infinity(sign, w) : MaxNormal(sign, w);
    }
    if (inexact) {
        e.fpsr |= FPSR_IXC;
        if (tiny) e.fpsr |= FPSR_UFC;         // ARM: "diminuto" se mira ANTES de redondear
    }
    return Pack(sign, biased, frac, f);
}

// Redondeo de la FPU del PC igual que FPCR mientras exista este objeto
struct RoundGuard {
    int old = 0;
    bool on = false;
    explicit RoundGuard(u32 rmode) {
        if (rmode != RN) {
            old = std::fegetround();
            std::fesetround(rmode == RP ? FE_UPWARD : rmode == RM ? FE_DOWNWARD : FE_TOWARDZERO);
            on = true;
        }
    }
    ~RoundGuard() { if (on) std::fesetround(old); }
};

// Un numero half (sin NaN) -> float exacto
float HalfToFloat(u64 h) {
    const Fmt f = FmtOf(16);
    const bool s = SignOf(h, 16);
    const u64 ef = ExpField(h, f), frac = FracField(h, f);
    float v;
    if (ef == 0)          v = std::ldexp(float(frac), -24);
    else if (ef == 31)    v = INFINITY;
    else                  v = std::ldexp(float(frac | 0x400), int(ef) - 25);
    return s ? -v : v;
}

// Resultado de la FPU del PC (en double, ya redondeado) -> half con redondeo r.
// 'host_inexact': la operacion en el PC ya fue inexacta (hay mas bits que se perdieron).
u64 DoubleToHalf(double v, Rounding r, Env& e, bool host_inexact) {
    if (std::isinf(v)) return Infinity(std::signbit(v), 16);
    if (v == 0) return Zero(std::signbit(v), 16);
    int ex;
    const double m = std::frexp(std::fabs(v), &ex);       // v = m * 2^ex, m en [0.5, 1)
    const u64 mant = u64(std::ldexp(m, 53));             // 53 bits exactos
    return RoundTo(std::signbit(v), ex - 53, mant, host_inexact, 16, r, e, true);
}

// Ejecuta f() sin dejar flags en la FPU del PC; devuelve si fue inexacta
template <typename F>
bool Isolated(F f) {
    std::fexcept_t saved;
    std::fegetexceptflag(&saved, FE_ALL_EXCEPT);
    std::feclearexcept(FE_ALL_EXCEPT);
    f();
    const bool inexact = std::fetestexcept(FE_INEXACT) != 0;
    std::fesetexceptflag(&saved, FE_ALL_EXCEPT);
    return inexact;
}

// Arreglos despues de una operacion de la FPU del PC: NaN de x86 -> NaN por defecto
// de ARM, y FZ en el resultado.
u64 FixResult(u64 r, unsigned w, Env& e) {
    const Type t = Classify(r, w);
    if (t == Type::QNaN || t == Type::SNaN) return DefaultNaN(w);
    if (t == Type::Denormal && FlushOn(w, e.fpcr)) {
        e.fpsr |= FPSR_UFC;
        return Zero(SignOf(r, w), w);
    }
    return r;
}

// Ejecuta una operacion en la FPU del PC con el modo de redondeo de FPCR.
// Sin FZ los flags se quedan en la FPU (se recogen con FoldHostFlags). Con FZ hay que
// mirarlos en el momento: si el resultado es tan pequeno que se vuelve 0, ARM solo
// marca UFC (no IXC, que el PC si habria marcado).
template <typename T, typename F>
u64 RunHost(unsigned w, u32 rm, Env& e, F f) {
    volatile T r;
    if (!FlushOn(w, e.fpcr)) {
        { RoundGuard g(rm); r = f(); }
        if constexpr (sizeof(T) == 8) return FixResult(B64(r), w, e);
        else                          return FixResult(B32(r), w, e);
    }
    std::fexcept_t saved;
    std::fegetexceptflag(&saved, FE_ALL_EXCEPT);
    std::feclearexcept(FE_ALL_EXCEPT);
    { RoundGuard g(rm); r = f(); }
    const int flags = std::fetestexcept(FE_ALL_EXCEPT);
    std::fesetexceptflag(&saved, FE_ALL_EXCEPT);
    u64 bits;
    if constexpr (sizeof(T) == 8) bits = B64(r);
    else                          bits = B32(r);
    const Type t = Classify(bits, w);
    if (t == Type::Denormal || (t == Type::Zero && (flags & FE_UNDERFLOW))) {
        e.fpsr |= FPSR_UFC;
        return Zero(SignOf(bits, w), w);
    }
    if (flags & FE_INVALID)   e.fpsr |= FPSR_IOC;
    if (flags & FE_DIVBYZERO) e.fpsr |= FPSR_DZC;
    if (flags & FE_OVERFLOW)  e.fpsr |= FPSR_OFC;
    if (flags & FE_INEXACT)   e.fpsr |= FPSR_IXC;
    return FixResult(bits, w, e);
}

// Operacion de 2 operandos ya sin NaN, en la FPU del PC
enum class Op2 { Add, Sub, Mul, Div };
u64 HostOp2(Op2 op, u64 a, u64 b, unsigned w, Env& e) {
    const u32 rm = RoundingMode(e.fpcr);
    if (w == 64) {
        const double x = F64(a), y = F64(b);
        return RunHost<double>(64, rm, e, [&]() -> double {
            switch (op) {
                case Op2::Add: return x + y;
                case Op2::Sub: return x - y;
                case Op2::Mul: return x * y;
                default:       return x / y;
            }
        });
    }
    if (w == 32) {
        const float x = F32(a), y = F32(b);
        return RunHost<float>(32, rm, e, [&]() -> float {
            switch (op) {
                case Op2::Add: return x + y;
                case Op2::Sub: return x - y;
                case Op2::Mul: return x * y;
                default:       return x / y;
            }
        });
    }
    // half: en double (las operaciones de half caben de sobra), luego redondear a half
    const double x = HalfToFloat(a), y = HalfToFloat(b);
    volatile double r = 0;
    const bool inexact = Isolated([&] {
        RoundGuard g(rm);
        switch (op) {
            case Op2::Add: r = x + y; break;
            case Op2::Sub: r = x - y; break;
            case Op2::Mul: r = x * y; break;
            default:       r = x / y; break;
        }
    });
    if (std::isnan(r)) { e.fpsr |= FPSR_IOC; return DefaultNaN(16); }
    if (op == Op2::Div && std::isinf(r) && !std::isinf(x)) e.fpsr |= FPSR_DZC;
    return DoubleToHalf(r, Rounding(rm), e, inexact);
}

// Camino rapido: ver Fast:: en fp_ops.hpp
inline bool FastFpcr(u32 fpcr) { return Fast::FpcrOk(fpcr); }
inline bool NaN32(u64 a) { return Fast::NaN32(a); }
inline bool NaN64(u64 a) { return Fast::NaN64(a); }
inline u64 Fix32(float r) { return Fast::Out32(r); }
inline u64 Fix64(double r) { return Fast::Out64(r); }

u64 Arith2(Op2 op, u64 a, u64 b, unsigned w, Env& e) {
    if (FastFpcr(e.fpcr)) {
        if (w == 32 && !NaN32(a) && !NaN32(b)) {
            const float x = F32(a), y = F32(b);
            switch (op) {
                case Op2::Add: return Fix32(x + y);
                case Op2::Sub: return Fix32(x - y);
                case Op2::Mul: return Fix32(x * y);
                default:       return Fix32(x / y);
            }
        }
        if (w == 64 && !NaN64(a) && !NaN64(b)) {
            const double x = F64(a), y = F64(b);
            switch (op) {
                case Op2::Add: return Fix64(x + y);
                case Op2::Sub: return Fix64(x - y);
                case Op2::Mul: return Fix64(x * y);
                default:       return Fix64(x / y);
            }
        }
    }
    u64 out;
    if (ProcessNaNs(a, b, w, e, out)) return out;
    return HostOp2(op, FlushInput(a, w, e), FlushInput(b, w, e), w, e);
}

} // namespace

// ============================================================================
//  Formatos
// ============================================================================

Type Classify(u64 b, unsigned w) {
    const Fmt f = FmtOf(w);
    const u64 ef = ExpField(b, f), frac = FracField(b, f);
    if (ef == 0) return frac == 0 ? Type::Zero : Type::Denormal;
    if (ef == Mask(f.ebits)) {
        if (frac == 0) return Type::Infinity;
        return (frac >> (f.fbits - 1)) & 1 ? Type::QNaN : Type::SNaN;
    }
    return Type::Normal;
}
bool IsNaN(u64 b, unsigned w) { const Type t = Classify(b, w); return t == Type::QNaN || t == Type::SNaN; }
u64 DefaultNaN(unsigned w) { const Fmt f = FmtOf(w); return Pack(false, Mask(f.ebits), 1ull << (f.fbits - 1), f); }
u64 Zero(bool s, unsigned w) { return u64(s) << (w - 1); }
u64 Infinity(bool s, unsigned w) { const Fmt f = FmtOf(w); return Pack(s, Mask(f.ebits), 0, f); }
u64 MaxNormal(bool s, unsigned w) { const Fmt f = FmtOf(w); return Pack(s, Mask(f.ebits) - 1, Mask(f.fbits), f); }
u64 Abs(u64 a, unsigned w) { return a & ~SignBit(w) & Mask(w); }
u64 Neg(u64 a, unsigned w) { return (a ^ SignBit(w)) & Mask(w); }

// ============================================================================
//  Aritmetica
// ============================================================================

u64 Add(u64 a, u64 b, unsigned w, Env& e) { return Arith2(Op2::Add, a, b, w, e); }
u64 Sub(u64 a, u64 b, unsigned w, Env& e) { return Arith2(Op2::Sub, a, b, w, e); }
u64 Mul(u64 a, u64 b, unsigned w, Env& e) { return Arith2(Op2::Mul, a, b, w, e); }
u64 Div(u64 a, u64 b, unsigned w, Env& e) { return Arith2(Op2::Div, a, b, w, e); }

u64 MulX(u64 a, u64 b, unsigned w, Env& e) {
    u64 out;
    if (ProcessNaNs(a, b, w, e, out)) return out;
    a = FlushInput(a, w, e);
    b = FlushInput(b, w, e);
    const Type ta = Classify(a, w), tb = Classify(b, w);
    if ((ta == Type::Infinity && tb == Type::Zero) || (ta == Type::Zero && tb == Type::Infinity)) {
        const Fmt f = FmtOf(w);   // +-2.0
        return Pack(SignOf(a, w) ^ SignOf(b, w), u64(f.bias + 1), 0, f);
    }
    return HostOp2(Op2::Mul, a, b, w, e);
}

u64 MulAdd(u64 addend, u64 a, u64 b, unsigned w, Env& e) {
    if (FastFpcr(e.fpcr)) {   // camino rapido (ver Arith2)
        if (w == 32 && !NaN32(addend) && !NaN32(a) && !NaN32(b))
            return Fix32(std::fma(F32(a), F32(b), F32(addend)));
        if (w == 64 && !NaN64(addend) && !NaN64(a) && !NaN64(b))
            return Fix64(std::fma(F64(a), F64(b), F64(addend)));
    }
    const Type tA = Classify(addend, w);
    const Type t1 = Classify(FlushInput(a, w, e), w), t2 = Classify(FlushInput(b, w, e), w);
    const bool inf1 = t1 == Type::Infinity, zero1 = t1 == Type::Zero;
    const bool inf2 = t2 == Type::Infinity, zero2 = t2 == Type::Zero;
    u64 out;
    const bool done = ProcessNaNs3(addend, a, b, w, e, out);
    if (tA == Type::QNaN && ((inf1 && zero2) || (zero1 && inf2))) {
        e.fpsr |= FPSR_IOC;
        return DefaultNaN(w);
    }
    if (done) return out;

    const u64 x = FlushInput(addend, w, e), y = FlushInput(a, w, e), z = FlushInput(b, w, e);
    const u32 rm = RoundingMode(e.fpcr);
    if (w == 64) return RunHost<double>(64, rm, e, [&] { return std::fma(F64(y), F64(z), F64(x)); });
    if (w == 32) return RunHost<float>(32, rm, e, [&] { return std::fma(F32(y), F32(z), F32(x)); });
    // half: en double truncando (RZ) + bit "sticky", como en StepFused
    volatile double r = 0;
    const bool inexact = Isolated([&] {
        RoundGuard g(RZ);
        r = std::fma(double(HalfToFloat(y)), double(HalfToFloat(z)), double(HalfToFloat(x)));
    });
    if (std::isnan(r)) { e.fpsr |= FPSR_IOC; return DefaultNaN(16); }
    if (r == 0) {   // resultado exacto 0 (en RZ el PC da +0): signo segun ARM
        const bool sp = SignOf(y, 16) != SignOf(z, 16);
        const bool zero_p = Classify(y, 16) == Type::Zero || Classify(z, 16) == Type::Zero;
        if (Classify(x, 16) == Type::Zero && zero_p && sp == SignOf(x, 16)) return Zero(sp, 16);
        return Zero(rm == RM, 16);
    }
    return DoubleToHalf(r, Rounding(rm), e, inexact);
}

u64 Sqrt(u64 a, unsigned w, Env& e) {
    if (FastFpcr(e.fpcr)) {   // camino rapido (ver Arith2)
        if (w == 32 && !NaN32(a)) return Fix32(std::sqrt(F32(a)));
        if (w == 64 && !NaN64(a)) return Fix64(std::sqrt(F64(a)));
    }
    const Type t = Classify(a, w);
    if (t == Type::QNaN || t == Type::SNaN) return ProcessNaN(a, w, e);
    a = FlushInput(a, w, e);
    if (Classify(a, w) == Type::Zero) return a;
    if (SignOf(a, w)) { e.fpsr |= FPSR_IOC; return DefaultNaN(w); }
    const u32 rm = RoundingMode(e.fpcr);
    if (w == 64) return RunHost<double>(64, rm, e, [&] { return std::sqrt(F64(a)); });
    if (w == 32) return RunHost<float>(32, rm, e, [&] { return std::sqrt(F32(a)); });
    volatile double r = 0;
    const bool inexact = Isolated([&] { RoundGuard g(rm); r = std::sqrt(double(HalfToFloat(a))); });
    return DoubleToHalf(r, Rounding(rm), e, inexact);
}

namespace {
// Valor de un numero (sin NaN) como double exacto, para comparar
double ValueOf(u64 b, unsigned w) {
    if (w == 64) return F64(b);
    if (w == 32) return double(F32(b));
    return double(HalfToFloat(b));
}

u64 MaxMin(u64 a, u64 b, unsigned w, Env& e, bool is_max) {
    u64 out;
    if (ProcessNaNs(a, b, w, e, out)) return out;
    a = FlushInput(a, w, e);
    b = FlushInput(b, w, e);
    const double va = ValueOf(a, w), vb = ValueOf(b, w);
    if (va == 0 && vb == 0) {
        // +0 > -0: el maximo solo es -0 si los dos lo son; el minimo, si alguno lo es
        const bool s = is_max ? (SignOf(a, w) && SignOf(b, w)) : (SignOf(a, w) || SignOf(b, w));
        return Zero(s, w);
    }
    return (is_max ? va > vb : va < vb) ? a : b;
}
} // namespace

u64 Max(u64 a, u64 b, unsigned w, Env& e) { return MaxMin(a, b, w, e, true); }
u64 Min(u64 a, u64 b, unsigned w, Env& e) { return MaxMin(a, b, w, e, false); }

u64 MaxNum(u64 a, u64 b, unsigned w, Env& e) {
    const Type ta = Classify(a, w), tb = Classify(b, w);
    if (ta == Type::QNaN && tb != Type::QNaN) a = Infinity(true, w);        // -inf: pierde siempre
    else if (ta != Type::QNaN && tb == Type::QNaN) b = Infinity(true, w);
    return Max(a, b, w, e);
}
u64 MinNum(u64 a, u64 b, unsigned w, Env& e) {
    const Type ta = Classify(a, w), tb = Classify(b, w);
    if (ta == Type::QNaN && tb != Type::QNaN) a = Infinity(false, w);       // +inf
    else if (ta != Type::QNaN && tb == Type::QNaN) b = Infinity(false, w);
    return Min(a, b, w, e);
}

// ============================================================================
//  Estimaciones (tablas de ARM)
// ============================================================================

namespace {
// RecipEstimate(a): a en 256..511 (0.5 .. 1.0) -> 256..511 (1.0 .. 2.0)
u32 RecipEstimateInt(u32 a) {
    a = a * 2 + 1;
    const u32 b = (1u << 19) / a;
    return (b + 1) / 2;
}
// RecipSqrtEstimate(a): a en 128..511 (0.25 .. 1.0) -> 256..511
u32 RecipSqrtEstimateInt(u32 a) {
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    u32 b = 512;
    while (u64(a) * (b + 1) * (b + 1) < (1ull << 28)) ++b;
    return (b + 1) / 2;
}
// La fraccion como un campo de 52 bits (como hace el manual para los 3 formatos)
u64 Frac52(u64 b, unsigned w) {
    const Fmt f = FmtOf(w);
    return FracField(b, f) << (52 - f.fbits);
}
} // namespace

u64 RecipEstimate(u64 a, unsigned w, Env& e) {
    const Fmt f = FmtOf(w);
    const Unpacked u = Unpack(a, w, e);
    if (u.type == Type::QNaN || u.type == Type::SNaN) return ProcessNaN(a, w, e);
    if (u.type == Type::Infinity) return Zero(u.sign, w);
    if (u.type == Type::Zero) { e.fpsr |= FPSR_DZC; return Infinity(u.sign, w); }

    const double av = std::fabs(ValueOf(a, w));
    // Constantes escritas tal cual: calcular 2^-1024 con ldexp marca "underflow" en la FPU
    // del PC con la libreria de MSVC, y ese flag acabaria en el FPSR del programa.
    const double tiny = w == 16 ? 0x1p-16 : w == 32 ? 0x1p-128 : 0x1p-1024;
    if (av < tiny) {
        const u32 rm = RoundingMode(e.fpcr);
        const bool to_inf = rm == RN || (rm == RP && !u.sign) || (rm == RM && u.sign);
        e.fpsr |= FPSR_OFC | FPSR_IXC;
        return to_inf ? Infinity(u.sign, w) : MaxNormal(u.sign, w);
    }
    const double big = w == 16 ? 0x1p14 : w == 32 ? 0x1p126 : 0x1p1022;
    if (FlushOn(w, e.fpcr) && av >= big) {
        e.fpsr |= FPSR_UFC;
        return Zero(u.sign, w);
    }

    u64 fraction = Frac52(a, w);
    s64 exp = s64(ExpField(a, f));
    if (exp == 0) {
        if (((fraction >> 51) & 1) == 0) { exp = -1; fraction = (fraction << 2) & Mask(52); }
        else fraction = (fraction << 1) & Mask(52);
    }
    const u32 scaled = u32(0x100 | (fraction >> 44));
    s64 result_exp = (w == 16 ? 29 : w == 32 ? 253 : 2045) - exp;
    const u32 estimate = RecipEstimateInt(scaled);
    fraction = u64(estimate & 0xFF) << 44;
    if (result_exp == 0) {
        fraction = (1ull << 51) | (fraction >> 1);
    } else if (result_exp == -1) {
        fraction = (1ull << 50) | (fraction >> 2);
        result_exp = 0;
    }
    return Pack(u.sign, u64(result_exp) & Mask(f.ebits), fraction >> (52 - f.fbits), f);
}

u64 RSqrtEstimate(u64 a, unsigned w, Env& e) {
    const Fmt f = FmtOf(w);
    const Unpacked u = Unpack(a, w, e);
    if (u.type == Type::QNaN || u.type == Type::SNaN) return ProcessNaN(a, w, e);
    if (u.type == Type::Zero) { e.fpsr |= FPSR_DZC; return Infinity(u.sign, w); }
    if (u.sign) { e.fpsr |= FPSR_IOC; return DefaultNaN(w); }
    if (u.type == Type::Infinity) return Zero(false, w);

    u64 fraction = Frac52(a, w);
    s64 exp = s64(ExpField(a, f));
    if (exp == 0) {
        while (((fraction >> 51) & 1) == 0) { fraction = (fraction << 1) & Mask(52); --exp; }
        fraction = (fraction << 1) & Mask(52);
    }
    const u32 scaled = (exp & 1) == 0 ? u32(0x100 | (fraction >> 44)) : u32(0x80 | (fraction >> 45));
    const s64 base = w == 16 ? 44 : w == 32 ? 380 : 3068;
    const s64 result_exp = (base - exp) / 2;
    const u32 estimate = RecipSqrtEstimateInt(scaled);
    const u64 frac = u64(estimate & 0xFF) << (f.fbits - 8);
    return Pack(false, u64(result_exp) & Mask(f.ebits), frac, f);
}

u32 URecipEstimate(u32 a) {
    if (!(a >> 31)) return 0xFFFFFFFFu;
    return (RecipEstimateInt(a >> 23) & 0x1FF) << 23;
}
u32 URSqrtEstimate(u32 a) {
    if ((a >> 30) == 0) return 0xFFFFFFFFu;
    return (RecipSqrtEstimateInt(a >> 23) & 0x1FF) << 23;
}

namespace {
// FRECPS / FRSQRTS: (k + a*b) / div con un solo redondeo; especial inf*0 = special
u64 StepFused(u64 a, u64 b, unsigned w, Env& e, double k, bool half_result) {
    a = Neg(a, w);
    u64 out;
    if (ProcessNaNs(a, b, w, e, out)) return out;
    a = FlushInput(a, w, e);
    b = FlushInput(b, w, e);
    const Type ta = Classify(a, w), tb = Classify(b, w);
    const Fmt f = FmtOf(w);
    if ((ta == Type::Infinity && tb == Type::Zero) || (ta == Type::Zero && tb == Type::Infinity))
        return half_result ? Pack(false, u64(f.bias), 1ull << (f.fbits - 1), f)   // 1.5
                           : Pack(false, u64(f.bias + 1), 0, f);                   // 2.0
    if (ta == Type::Infinity || tb == Type::Infinity) return Infinity(SignOf(a, w) ^ SignOf(b, w), w);

    const u32 rm = RoundingMode(e.fpcr);
    if (w == 64) {
        if (!half_result) return RunHost<double>(64, rm, e, [&] { return std::fma(F64(a), F64(b), k); });
        // FRSQRTS: ARM divide entre 2 antes de redondear. (3 - a*b) / 2 = 1.5 - (a/2)*b, partiendo
        // a la mitad el operando mas grande (exacto si no es diminuto). Asi no se desborda
        // en un paso intermedio.
        double x = F64(a), y = F64(b);
        if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
        if (std::fabs(x) >= 0x1p-1020) return RunHost<double>(64, rm, e, [&] { return std::fma(x * 0.5, y, 1.5); });
        return RunHost<double>(64, rm, e, [&] { return std::fma(x, y, 3.0) * 0.5; });
    }
    // 16 y 32: en double (el producto es exacto) truncando (RZ) y guardando si se
    // perdieron bits; luego un solo redondeo al formato con esos bits como "sticky".
    // (Redondear dos veces, primero a double y luego a float, falla en casos raros.)
    volatile double r = 0;
    const bool inexact = Isolated([&] {
        RoundGuard g(RZ);
        r = std::fma(ValueOf(a, w), ValueOf(b, w), k);
        if (half_result) r = r * 0.5;
    });
    if (r == 0) return Zero(rm == RM, w);
    if (w == 16) return DoubleToHalf(r, Rounding(rm), e, inexact);
    int ex;
    const double m = std::frexp(std::fabs(r), &ex);
    return RoundTo(std::signbit(r), ex - 53, u64(std::ldexp(m, 53)), inexact, 32, Rounding(rm), e, true);
}
} // namespace

u64 RecipStepFused(u64 a, u64 b, unsigned w, Env& e) { return StepFused(a, b, w, e, 2.0, false); }
u64 RSqrtStepFused(u64 a, u64 b, unsigned w, Env& e) { return StepFused(a, b, w, e, 3.0, true); }

u64 RecpX(u64 a, unsigned w, Env& e) {
    const Fmt f = FmtOf(w);
    const Type t = Classify(a, w);
    if (t == Type::QNaN || t == Type::SNaN) return ProcessNaN(a, w, e);
    if (t == Type::Denormal && FlushOn(w, e.fpcr) && w != 16) e.fpsr |= FPSR_IDC;
    const u64 exp = ExpField(a, f);
    const u64 max_exp = Mask(f.ebits) - 1;
    const u64 rexp = exp == 0 ? max_exp : (~exp & Mask(f.ebits));
    return Pack(SignOf(a, w), rexp, 0, f);
}

// ============================================================================
//  Comparaciones
// ============================================================================

u32 Compare(u64 a, u64 b, unsigned w, bool signal_qnan, Env& e) {
    const Type ta = Classify(a, w), tb = Classify(b, w);
    if (ta == Type::QNaN || ta == Type::SNaN || tb == Type::QNaN || tb == Type::SNaN) {
        NoteDenormals({a, b}, w, e);
        if (ta == Type::SNaN || tb == Type::SNaN || signal_qnan) e.fpsr |= FPSR_IOC;
        return 0b0011;
    }
    const double va = ValueOf(FlushInput(a, w, e), w), vb = ValueOf(FlushInput(b, w, e), w);
    if (va == vb) return 0b0110;
    return va < vb ? 0b1000 : 0b0010;
}
bool CompareEQ(u64 a, u64 b, unsigned w, Env& e) {
    const Type ta = Classify(a, w), tb = Classify(b, w);
    if (ta == Type::QNaN || ta == Type::SNaN || tb == Type::QNaN || tb == Type::SNaN) {
        NoteDenormals({a, b}, w, e);
        if (ta == Type::SNaN || tb == Type::SNaN) e.fpsr |= FPSR_IOC;
        return false;
    }
    return ValueOf(FlushInput(a, w, e), w) == ValueOf(FlushInput(b, w, e), w);
}
bool CompareGE(u64 a, u64 b, unsigned w, Env& e) {
    if (IsNaN(a, w) || IsNaN(b, w)) { NoteDenormals({a, b}, w, e); e.fpsr |= FPSR_IOC; return false; }
    return ValueOf(FlushInput(a, w, e), w) >= ValueOf(FlushInput(b, w, e), w);
}
bool CompareGT(u64 a, u64 b, unsigned w, Env& e) {
    if (IsNaN(a, w) || IsNaN(b, w)) { NoteDenormals({a, b}, w, e); e.fpsr |= FPSR_IOC; return false; }
    return ValueOf(FlushInput(a, w, e), w) > ValueOf(FlushInput(b, w, e), w);
}

// ============================================================================
//  Redondeo a entero y conversiones (todo en software, bit a bit)
// ============================================================================

namespace {
// Parte entera de mant * 2^exp redondeada con r. 'inexact' si se perdio algo.
// 'overflow' si no cabe en 64 bits.
u64 IntegerPart(bool sign, int exp, u64 mant, Rounding r, bool& inexact, bool& overflow) {
    inexact = overflow = false;
    if (exp == 0) return mant;
    if (exp > 0) {
        if (exp >= 64 || (mant >> (64 - exp)) != 0) { overflow = true; return ~0ull; }
        return mant << exp;
    }
    const int shift = -exp;
    u64 intpart;
    bool round_bit, sticky;
    if (shift >= 64) {
        intpart = 0;
        round_bit = shift == 64 && ((mant >> 63) & 1);
        sticky = shift == 64 ? (mant << 1) != 0 : mant != 0;
    } else {
        intpart = mant >> shift;
        const u64 rem = mant & Mask(unsigned(shift));
        round_bit = (rem >> (shift - 1)) & 1;
        sticky = (rem & Mask(unsigned(shift - 1))) != 0;
    }
    inexact = round_bit || sticky;
    bool up = false;
    switch (r) {
        case RN: up = round_bit && (sticky || (intpart & 1)); break;
        case RA: up = round_bit; break;
        case RP: up = inexact && !sign; break;
        case RM: up = inexact && sign; break;
        default: break;
    }
    if (up) {
        if (intpart == ~0ull) { overflow = true; return ~0ull; }
        ++intpart;
    }
    return intpart;
}
} // namespace

u64 RoundInt(u64 a, unsigned w, Rounding r, bool exact, Env& e) {
    const Unpacked u = Unpack(a, w, e);
    if (u.type == Type::QNaN || u.type == Type::SNaN) return ProcessNaN(a, w, e);
    if (u.type == Type::Infinity) return Infinity(u.sign, w);
    if (u.type == Type::Zero) return Zero(u.sign, w);
    if (u.exp >= 0) return a;                       // ya es entero
    bool inexact, overflow;
    const u64 v = IntegerPart(u.sign, u.exp, u.mant, r, inexact, overflow);
    if (exact && inexact) e.fpsr |= FPSR_IXC;
    if (v == 0) return Zero(u.sign, w);
    u64 dummy = 0;
    Env quiet{e.fpcr, dummy};
    return RoundTo(u.sign, 0, v, false, w, RN, quiet, false);   // exacto: ningun flag
}

u64 ToFixed(u64 a, unsigned w, unsigned fbits, bool is_unsigned, Rounding r, unsigned out_bits, Env& e) {
    const Unpacked u = Unpack(a, w, e);
    const u64 umax = Mask(out_bits);
    const u64 smax = Mask(out_bits - 1), smin = 1ull << (out_bits - 1);   // como bits
    auto saturate = [&](bool negative) -> u64 {
        e.fpsr |= FPSR_IOC;
        if (is_unsigned) return negative ? 0 : umax;
        return negative ? smin : smax;
    };
    if (u.type == Type::QNaN || u.type == Type::SNaN) { e.fpsr |= FPSR_IOC; return 0; }
    if (u.type == Type::Infinity) return saturate(u.sign);
    if (u.type == Type::Zero) return 0;

    bool inexact, overflow;
    const u64 mag = IntegerPart(u.sign, u.exp + int(fbits), u.mant, r, inexact, overflow);
    if (overflow) return saturate(u.sign);
    u64 result;
    if (is_unsigned) {
        if (u.sign && mag != 0) return saturate(true);
        if (mag > umax) return saturate(false);
        result = mag;
    } else {
        if (u.sign) {
            if (mag > smin) return saturate(true);
            result = (0 - mag) & umax;
        } else {
            if (mag > smax) return saturate(false);
            result = mag;
        }
    }
    if (inexact) e.fpsr |= FPSR_IXC;
    return result;
}

u64 FixedToFP(u64 v, unsigned in_bits, unsigned fbits, bool is_unsigned, unsigned w, Rounding r, Env& e) {
    v &= Mask(in_bits);
    bool sign = false;
    u64 mag = v;
    if (!is_unsigned && ((v >> (in_bits - 1)) & 1)) {
        sign = true;
        mag = (0 - v) & Mask(in_bits);
        if (mag == 0) mag = 1ull << (in_bits - 1);   // el minimo negativo
    }
    if (mag == 0) return Zero(false, w);
    return RoundTo(sign, -int(fbits), mag, false, w, r, e, true);
}

u64 Convert(u64 a, unsigned from_w, unsigned to_w, Rounding r, Env& e) {
    const Fmt ff = FmtOf(from_w), tf = FmtOf(to_w);
    const bool ahp = (e.fpcr & FPCR_AHP) != 0;
    const bool sign = SignOf(a, from_w);

    // Origen half con AHP: no hay infinitos ni NaN (exponente 31 es un numero normal)
    if (from_w == 16 && ahp && ExpField(a, ff) == 31) {
        const u64 mant = FracField(a, ff) | 0x400;
        return RoundTo(sign, 31 - ff.bias - 10, mant, false, to_w, r, e, to_w != 16);
    }
    // FZ16 no se aplica a las conversiones; FZ si (origen de 32/64 bits)
    const Unpacked u = Unpack(a, from_w, e, from_w != 16);
    if (u.type == Type::QNaN || u.type == Type::SNaN) {
        if (to_w == 16 && ahp) { e.fpsr |= FPSR_IOC; return Zero(sign, 16); }
        if (u.type == Type::SNaN) e.fpsr |= FPSR_IOC;
        if (e.fpcr & FPCR_DN) return DefaultNaN(to_w);
        // FPConvertNaN: se conserva el signo y los bits altos de la fraccion
        const u64 frac52 = FracField(a, ff) << (52 - ff.fbits);
        const u64 frac = ((frac52 >> (52 - tf.fbits)) | (1ull << (tf.fbits - 1))) & Mask(tf.fbits);
        return Pack(sign, Mask(tf.ebits), frac, tf);
    }
    if (u.type == Type::Infinity) {
        if (to_w == 16 && ahp) { e.fpsr |= FPSR_IOC; return (u64(sign) << 15) | 0x7FFF; }
        return Infinity(sign, to_w);
    }
    if (u.type == Type::Zero) return Zero(sign, to_w);
    return RoundTo(sign, u.exp, u.mant, false, to_w, r, e, to_w != 16, to_w == 16 && ahp);
}

// ============================================================================
//  Flags de la FPU del PC
// ============================================================================

void FoldHostFlags(u64& fpsr) {
    const int f = std::fetestexcept(FE_ALL_EXCEPT);
    if (f & FE_INVALID)   fpsr |= FPSR_IOC;
    if (f & FE_DIVBYZERO) fpsr |= FPSR_DZC;
    if (f & FE_OVERFLOW)  fpsr |= FPSR_OFC;
    if (f & FE_UNDERFLOW) fpsr |= FPSR_UFC;
    if (f & FE_INEXACT)   fpsr |= FPSR_IXC;
    std::feclearexcept(FE_ALL_EXCEPT);
}
void ClearHostFlags() { std::feclearexcept(FE_ALL_EXCEPT); }

} // namespace NeXo2::Core::FP
