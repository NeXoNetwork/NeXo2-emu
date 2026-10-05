// Coma flotante escalar y SIMD escalar (bits 28..25 = 1111).
// Instrucciones: FMOV (registro, inmediato, general<->FP), FABS, FNEG, FSQRT, FCVT,
// FRINT*, FADD, FSUB, FMUL, FDIV, FMAX/FMIN(NM), FNMUL, FMADD/FMSUB/FNMADD/FNMSUB,
// FCMP/FCMPE, FCCMP, FCSEL, SCVTF/UCVTF, FCVT[N/P/M/Z/A][S/U],
// y en "SIMD escalar": CMxx d,#0, ADD/SUB d, SHL/USHR/SSHR d, ADDP d, DUP (mov d0, v1.d[1]).
//
// Se usa la coma flotante del PC (IEEE 754, igual que ARM) con redondeo al par mas
// cercano. Los modos de redondeo de FPCR y los flags de FPSR todavia no se emulan.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"
#include <cmath>
#include <cstring>
#include <limits>

namespace NeXo2::Core {

using namespace NeXo2::Common;

namespace {

float  AsFloat(u64 bits)  { float f;  u32 b = static_cast<u32>(bits); std::memcpy(&f, &b, 4); return f; }
double AsDouble(u64 bits) { double d; std::memcpy(&d, &bits, 8); return d; }
u64 Bits32(float f)  { u32 b; std::memcpy(&b, &f, 4); return b; }
u64 Bits64(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }

// VFPExpandImm del manual: el inmediato de 8 bits de "fmov d0, #1.5"
u64 ExpandFpImm(u32 imm8, bool is_double) {
    const u64 sign = (imm8 >> 7) & 1;
    const u64 b6   = (imm8 >> 6) & 1;
    const u64 exp_low = (imm8 >> 4) & 3;
    const u64 frac = imm8 & 0xF;
    if (is_double) {
        const u64 exp = ((b6 ^ 1) << 10) | ((b6 ? 0xFFULL : 0) << 2) | exp_low;
        return (sign << 63) | (exp << 52) | (frac << 48);
    }
    const u64 exp = ((b6 ^ 1) << 7) | ((b6 ? 0x1FULL : 0) << 2) | exp_low;
    return (sign << 31) | (exp << 23) | (frac << 19);
}

// Redondeo segun el modo de la instruccion
enum class Round { NearestEven, PlusInf, MinusInf, Zero, NearestAway };
double ApplyRound(double v, Round r) {
    switch (r) {
        case Round::NearestEven: return std::nearbyint(v); // el modo por defecto del PC es "al par"
        case Round::PlusInf:     return std::ceil(v);
        case Round::MinusInf:    return std::floor(v);
        case Round::Zero:        return std::trunc(v);
        default:                 return std::round(v);     // empates lejos del cero
    }
}

// Coma flotante -> entero con saturacion (como FCVTZS y compania)
u64 FpToInt(double v, Round r, bool is_signed, unsigned bits) {
    if (std::isnan(v)) return 0;
    const double t = ApplyRound(v, r);
    if (is_signed) {
        const double max = std::ldexp(1.0, int(bits) - 1);  // 2^(bits-1)
        if (t >= max)  return Ones(bits - 1);                // INT_MAX
        if (t < -max)  return (1ULL << (bits - 1)) & Ones(bits); // INT_MIN
        return static_cast<u64>(static_cast<s64>(t)) & Ones(bits);
    }
    if (t <= 0) return 0;
    if (t >= std::ldexp(1.0, int(bits))) return Ones(bits);
    return static_cast<u64>(t) & Ones(bits);
}

// Maximo/minimo con las reglas de ARM: NaN gana (FMAX) o pierde (FMAXNM); +0 > -0
double FpMax(double a, double b, bool nm) {
    if (std::isnan(a) || std::isnan(b)) {
        if (nm && std::isnan(a) && !std::isnan(b)) return b;
        if (nm && std::isnan(b) && !std::isnan(a)) return a;
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
double FpMin(double a, double b, bool nm) {
    if (std::isnan(a) || std::isnan(b)) {
        if (nm && std::isnan(a) && !std::isnan(b)) return b;
        if (nm && std::isnan(b) && !std::isnan(a)) return a;
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (a == 0 && b == 0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}

} // namespace

// Reparto: coma flotante escalar, SIMD escalar o SIMD vectorial
bool Interpreter::ExecSimdFp(u32 instr) {
    const u32 b28_25 = Bits(instr, 25, 4);
    if (b28_25 == 0b0111) return ExecSimdVector(instr);              // 0 Q U 0111 ...
    if (b28_25 == 0b1111 && Bit(instr, 30) == 0 && Bit(instr, 29) == 0)
        return ExecFloatingPoint(instr);                              // FP escalar (bit 31 = sf en conversiones)
    if (b28_25 == 0b1111 && Bit(instr, 30) == 1)
        return ExecSimdVector(instr);                                 // SIMD escalar (01 U 1111 ...)
    return false;
}

bool Interpreter::ExecFloatingPoint(u32 instr) {
    const u32 type = Bits(instr, 22, 2);     // 00 = float (s), 01 = double (d)
    if (type > 1 && !(type == 2 && Bits(instr, 10, 6) == 0)) return false; // half: pendiente
    const bool dbl = (type == 1);
    const unsigned bytes = dbl ? 8 : 4;
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);

    auto get = [&](u32 r) -> double { return dbl ? AsDouble(Vreg(r).lo) : double(AsFloat(Vreg(r).lo)); };
    auto put = [&](u32 r, double v) { SetVScalar(r, dbl ? Bits64(v) : Bits32(static_cast<float>(v)), bytes); };

    // --- Fusionadas de 3 operandos: FMADD, FMSUB, FNMADD, FNMSUB (bits 28..24 = 11111) ---
    if (Bits(instr, 24, 5) == 0b11111) {
        const u32 ra = Bits(instr, 10, 5);
        const bool o1 = Bit(instr, 21), o0 = Bit(instr, 15);
        if (dbl) {
            const double n = get(rn), m = get(rm), a = get(ra);
            double r;
            if (!o1 && !o0)     r = std::fma(n, m, a);    // FMADD:  a + n*m
            else if (!o1 && o0) r = std::fma(-n, m, a);   // FMSUB:  a - n*m
            else if (o1 && !o0) r = std::fma(-n, m, -a);  // FNMADD: -a - n*m
            else                r = std::fma(n, m, -a);   // FNMSUB: -a + n*m
            put(rd, r);
        } else {
            const float n = AsFloat(Vreg(rn).lo), m = AsFloat(Vreg(rm).lo), a = AsFloat(Vreg(ra).lo);
            float r;
            if (!o1 && !o0)     r = std::fma(n, m, a);
            else if (!o1 && o0) r = std::fma(-n, m, a);
            else if (o1 && !o0) r = std::fma(-n, m, -a);
            else                r = std::fma(n, m, -a);
            SetVScalar(rd, Bits32(r), 4);
        }
        return true;
    }

    if (Bits(instr, 24, 5) != 0b11110) return false;
    if (Bit(instr, 21) == 0) return false; // conversiones de coma fija: pendiente

    // --- Conversiones entero <-> coma flotante y FMOV con registros generales ---
    if (Bits(instr, 10, 6) == 0) {
        const bool sf = Bit(instr, 31);
        const u32 rmode = Bits(instr, 19, 2), opcode = Bits(instr, 16, 3);
        const unsigned ibits = sf ? 64 : 32;

        if (opcode == 0b110 || opcode == 0b111) {             // FMOV
            const bool to_fp = (opcode == 0b111);
            if (type == 2 && sf && rmode == 1) {              // FMOV Xd <-> Vn.D[1]
                if (to_fp) Vreg(rd).hi = X(rn);
                else       SetX(rd, Vreg(rn).hi);
                return true;
            }
            if (rmode != 0 || type > 1) return false;
            if (to_fp) SetVScalar(rd, X(rn, sf), bytes);       // fmov d0, x1
            else       SetX(rd, Vreg(rn).Get(0, bytes), sf);   // fmov x0, d1
            return true;
        }
        if (type > 1) return false;
        if ((opcode == 0b010 || opcode == 0b011) && rmode == 0) {   // SCVTF / UCVTF
            const u64 raw = X(rn, sf);
            const double v = (opcode == 0b010) ? double(SignExtend(raw, ibits)) : double(raw);
            if (dbl) put(rd, v);
            else     SetVScalar(rd, Bits32((opcode == 0b010) ? float(SignExtend(raw, ibits)) : float(raw)), 4);
            return true;
        }
        if (opcode <= 0b001 || opcode == 0b100 || opcode == 0b101) { // FCVT{N,P,M,Z,A}{S,U}
            const bool is_signed = (opcode & 1) == 0;
            Round r;
            if (opcode >= 0b100) { if (rmode != 0) return false; r = Round::NearestAway; }
            else r = static_cast<Round>(rmode);   // 00 N, 01 P, 10 M, 11 Z
            SetX(rd, FpToInt(get(rn), r, is_signed, ibits), sf);
            return true;
        }
        return false;
    }

    // --- 1 operando: FMOV, FABS, FNEG, FSQRT, FCVT, FRINT* ---
    if (Bits(instr, 10, 5) == 0b10000) {
        const u32 opcode = Bits(instr, 15, 6);
        const u64 raw = Vreg(rn).Get(0, bytes);
        const u64 sign = dbl ? (1ULL << 63) : (1ULL << 31);
        switch (opcode) {
            case 0b000000: SetVScalar(rd, raw, bytes);         return true; // FMOV
            case 0b000001: SetVScalar(rd, raw & ~sign, bytes); return true; // FABS
            case 0b000010: SetVScalar(rd, raw ^ sign, bytes);  return true; // FNEG
            case 0b000011: put(rd, std::sqrt(get(rn)));        return true; // FSQRT
            case 0b000100: SetVScalar(rd, Bits32(static_cast<float>(get(rn))), 4); return true; // FCVT -> s
            case 0b000101: SetVScalar(rd, Bits64(get(rn)), 8);                    return true; // FCVT -> d
            case 0b001000: put(rd, ApplyRound(get(rn), Round::NearestEven)); return true; // FRINTN
            case 0b001001: put(rd, ApplyRound(get(rn), Round::PlusInf));     return true; // FRINTP
            case 0b001010: put(rd, ApplyRound(get(rn), Round::MinusInf));    return true; // FRINTM
            case 0b001011: put(rd, ApplyRound(get(rn), Round::Zero));        return true; // FRINTZ
            case 0b001100: put(rd, ApplyRound(get(rn), Round::NearestAway)); return true; // FRINTA
            case 0b001110:                                                                 // FRINTX
            case 0b001111: put(rd, ApplyRound(get(rn), Round::NearestEven)); return true; // FRINTI
            default: return false;
        }
    }

    // --- Comparar: FCMP / FCMPE (con registro o con 0.0) ---
    if (Bits(instr, 10, 4) == 0b1000) {
        const bool with_zero = Bit(instr, 3);
        const double a = get(rn), b = with_zero ? 0.0 : get(rm);
        auto& f = m_state.flags;
        if (std::isnan(a) || std::isnan(b)) { f.n = false; f.z = false; f.c = true;  f.v = true;  }
        else if (a == b)                    { f.n = false; f.z = true;  f.c = true;  f.v = false; }
        else if (a < b)                     { f.n = true;  f.z = false; f.c = false; f.v = false; }
        else                                { f.n = false; f.z = false; f.c = true;  f.v = false; }
        return true;
    }

    // --- Inmediato: fmov d0, #1.5 ---
    if (Bits(instr, 10, 3) == 0b100) {
        SetVScalar(rd, ExpandFpImm(Bits(instr, 13, 8), dbl), bytes);
        return true;
    }

    const u32 op2 = Bits(instr, 10, 2);
    // --- FCCMP / FCCMPE: compara si se cumple la condicion; si no, NZCV = #nzcv ---
    if (op2 == 0b01) {
        if (ConditionHolds(Bits(instr, 12, 4))) {
            const double a = get(rn), b = get(rm);
            auto& f = m_state.flags;
            if (std::isnan(a) || std::isnan(b)) { f.n = false; f.z = false; f.c = true;  f.v = true;  }
            else if (a == b)                    { f.n = false; f.z = true;  f.c = true;  f.v = false; }
            else if (a < b)                     { f.n = true;  f.z = false; f.c = false; f.v = false; }
            else                                { f.n = false; f.z = false; f.c = true;  f.v = false; }
        } else {
            m_state.SetNZCV(u64(Bits(instr, 0, 4)) << 28);
        }
        return true;
    }

    // --- 2 operandos: FMUL, FDIV, FADD, FSUB, FMAX, FMIN, FMAXNM, FMINNM, FNMUL ---
    if (op2 == 0b10) {
        const u32 opcode = Bits(instr, 12, 4);
        if (!dbl) { // en float hay que operar en float para redondear igual que ARM
            const float a = AsFloat(Vreg(rn).lo), b = AsFloat(Vreg(rm).lo);
            float r;
            switch (opcode) {
                case 0b0000: r = a * b; break;
                case 0b0001: r = a / b; break;
                case 0b0010: r = a + b; break;
                case 0b0011: r = a - b; break;
                case 0b0100: r = float(FpMax(a, b, false)); break;
                case 0b0101: r = float(FpMin(a, b, false)); break;
                case 0b0110: r = float(FpMax(a, b, true));  break;
                case 0b0111: r = float(FpMin(a, b, true));  break;
                case 0b1000: r = -(a * b); break;
                default: return false;
            }
            SetVScalar(rd, Bits32(r), 4);
            return true;
        }
        const double a = get(rn), b = get(rm);
        double r;
        switch (opcode) {
            case 0b0000: r = a * b; break;
            case 0b0001: r = a / b; break;
            case 0b0010: r = a + b; break;
            case 0b0011: r = a - b; break;
            case 0b0100: r = FpMax(a, b, false); break;
            case 0b0101: r = FpMin(a, b, false); break;
            case 0b0110: r = FpMax(a, b, true);  break;
            case 0b0111: r = FpMin(a, b, true);  break;
            case 0b1000: r = -(a * b); break;
            default: return false;
        }
        put(rd, r);
        return true;
    }

    // --- FCSEL: Vd = condicion ? Vn : Vm ---
    if (op2 == 0b11) {
        const u64 v = ConditionHolds(Bits(instr, 12, 4)) ? Vreg(rn).Get(0, bytes) : Vreg(rm).Get(0, bytes);
        SetVScalar(rd, v, bytes);
        return true;
    }
    return false;
}

} // namespace NeXo2::Core
