// Coma flotante escalar (bits 28..24 = 11110 / 11111), grupos del manual de ARM:
//   conversion con coma fija, conversion con enteros (y FMOV con registros generales),
//   1 operando (FMOV, FABS, FNEG, FSQRT, FCVT, FRINT*), comparacion, inmediato,
//   comparacion condicional, 2 operandos (FADD, FMUL, FMAX...), seleccion condicional
//   y 3 operandos (FMADD...). En los tres formatos: half (16), single (32), double (64).
// Todo el calculo va por fp_ops.hpp, que aplica las reglas exactas de ARM.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"
#include "fp_ops.hpp"

namespace NeXo2::Core {

using namespace NeXo2::Common;


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
    // Formato: type 00 = single (32), 01 = double (64), 11 = half (16), 10 = no existe
    const u32 type = Bits(instr, 22, 2);
    const unsigned w = type == 0 ? 32 : type == 1 ? 64 : type == 3 ? 16 : 0;
    const unsigned bytes = w / 8;
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    FP::Env env{static_cast<u32>(m_state.fpcr), m_state.fpsr};
    const FP::Rounding fpcr_round = FP::Rounding(FP::RoundingMode(env.fpcr));
    auto get = [&](u32 r) -> u64 { return Vreg(r).Get(0, bytes); };
    auto put = [&](u32 r, u64 v) { SetVScalar(r, v, bytes); };

    // --- 3 operandos: FMADD, FMSUB, FNMADD, FNMSUB (bits 28..24 = 11111) ---
    if (Bits(instr, 24, 5) == 0b11111) {
        if (Bit(instr, 31) || Bit(instr, 29) || w == 0) return false;
        const u32 ra = Bits(instr, 10, 5);
        const bool o1 = Bit(instr, 21), o0 = Bit(instr, 15);
        u64 a = get(ra), n = get(rn);
        const u64 m = get(rm);
        // Como el manual: las negaciones se aplican a los operandos ANTES (afecta a los NaN)
        if (o1) a = FP::Neg(a, w);          // FNMADD / FNMSUB: -a
        if (o0 != o1) n = FP::Neg(n, w);    // FMSUB / FNMADD: -n*m
        put(rd, FP::MulAdd(a, n, m, w, env));
        return true;
    }
    if (Bits(instr, 24, 5) != 0b11110 || Bit(instr, 30)) return false;

    // --- Conversion con coma fija: SCVTF/UCVTF/FCVTZS/FCVTZU con #fbits (bit 21 = 0) ---
    if (Bit(instr, 21) == 0) {
        const bool sf = Bit(instr, 31);
        const u32 rmode = Bits(instr, 19, 2), opcode = Bits(instr, 16, 3), scale = Bits(instr, 10, 6);
        if (Bit(instr, 29) || w == 0 || (!sf && scale < 32)) return false;
        const unsigned fbits = 64 - scale, ibits = sf ? 64 : 32;
        if (rmode == 0b00 && (opcode == 0b010 || opcode == 0b011)) {          // SCVTF / UCVTF
            put(rd, FP::FixedToFP(X(rn, sf), ibits, fbits, opcode == 0b011, w, fpcr_round, env));
            return true;
        }
        if (rmode == 0b11 && (opcode == 0b000 || opcode == 0b001)) {          // FCVTZS / FCVTZU
            SetX(rd, FP::ToFixed(get(rn), w, fbits, opcode == 0b001, FP::RZ, ibits, env), sf);
            return true;
        }
        return false;
    }

    // --- Conversion con enteros y FMOV con registros generales (bits 15..10 = 000000) ---
    if (Bits(instr, 10, 6) == 0) {
        const bool sf = Bit(instr, 31);
        const u32 rmode = Bits(instr, 19, 2), opcode = Bits(instr, 16, 3);
        const unsigned ibits = sf ? 64 : 32;
        if (Bit(instr, 29)) return false;

        if (opcode == 0b110 || opcode == 0b111) {                              // FMOV
            const bool to_fp = (opcode == 0b111);
            if (type == 2 && sf && rmode == 1) {                               // FMOV Xd <-> Vn.D[1]
                if (to_fp) Vreg(rd).hi = X(rn);
                else       SetX(rd, Vreg(rn).hi);
                return true;
            }
            if (rmode != 0 || w == 0) return false;
            if (w == 16 || (w == 32 && !sf) || (w == 64 && sf)) {
                if (to_fp) put(rd, X(rn, sf) & ((w == 64) ? ~0ull : ((1ull << w) - 1)));
                else       SetX(rd, get(rn), sf);
                return true;
            }
            return false;
        }
        if (w == 0) return false;
        if (opcode == 0b010 || opcode == 0b011) {                              // SCVTF / UCVTF
            if (rmode != 0) return false;
            put(rd, FP::FixedToFP(X(rn, sf), ibits, 0, opcode == 0b011, w, fpcr_round, env));
            return true;
        }
        if (opcode <= 0b001 || opcode == 0b100 || opcode == 0b101) {          // FCVT{N,P,M,Z,A}{S,U}
            FP::Rounding r;
            if (opcode >= 0b100) { if (rmode != 0) return false; r = FP::RA; }
            else r = FP::Rounding(rmode);                                       // 00 N, 01 P, 10 M, 11 Z
            SetX(rd, FP::ToFixed(get(rn), w, 0, (opcode & 1) != 0, r, ibits, env), sf);
            return true;
        }
        return false;
    }

    if (Bit(instr, 31) || Bit(instr, 29) || w == 0) return false;

    // --- 1 operando (bits 14..10 = 10000): FMOV, FABS, FNEG, FSQRT, FCVT, FRINT* ---
    if (Bits(instr, 10, 5) == 0b10000) {
        const u32 opcode = Bits(instr, 15, 6);
        const u64 v = get(rn);
        switch (opcode) {
            case 0b000000: put(rd, v); return true;                                     // FMOV
            case 0b000001: put(rd, FP::Abs(v, w)); return true;                         // FABS
            case 0b000010: put(rd, FP::Neg(v, w)); return true;                         // FNEG
            case 0b000011: put(rd, FP::Sqrt(v, w, env)); return true;                   // FSQRT
            case 0b000100: case 0b000101: case 0b000111: {                             // FCVT
                const unsigned to = opcode == 0b000100 ? 32 : opcode == 0b000101 ? 64 : 16;
                if (to == w) return false;
                SetVScalar(rd, FP::Convert(v, w, to, fpcr_round, env), to / 8);
                return true;
            }
            case 0b001000: put(rd, FP::RoundInt(v, w, FP::RN, false, env)); return true;   // FRINTN
            case 0b001001: put(rd, FP::RoundInt(v, w, FP::RP, false, env)); return true;   // FRINTP
            case 0b001010: put(rd, FP::RoundInt(v, w, FP::RM, false, env)); return true;   // FRINTM
            case 0b001011: put(rd, FP::RoundInt(v, w, FP::RZ, false, env)); return true;   // FRINTZ
            case 0b001100: put(rd, FP::RoundInt(v, w, FP::RA, false, env)); return true;   // FRINTA
            case 0b001110: put(rd, FP::RoundInt(v, w, fpcr_round, true, env)); return true;  // FRINTX
            case 0b001111: put(rd, FP::RoundInt(v, w, fpcr_round, false, env)); return true; // FRINTI
            default: return false;
        }
    }

    const auto set_nzcv = [&](u32 nzcv) { m_state.SetNZCV(u64(nzcv) << 28); };

    // --- Comparar (bits 13..10 = 1000): FCMP / FCMPE, con registro o con 0.0 ---
    if (Bits(instr, 10, 4) == 0b1000) {
        if (Bits(instr, 14, 2) != 0 || Bits(instr, 0, 3) != 0) return false;
        const bool with_zero = Bit(instr, 3), signal = Bit(instr, 4);
        set_nzcv(FP::Compare(get(rn), with_zero ? 0 : get(rm), w, signal, env));
        return true;
    }

    // --- Inmediato (bits 12..10 = 100): fmov d0, #1.5 ---
    if (Bits(instr, 10, 3) == 0b100) {
        if (Bits(instr, 5, 5) != 0) return false;
        const u32 imm8 = Bits(instr, 13, 8);
        // VFPExpandImm: signo, exponente "NOT(b):b repetido:cd", fraccion efgh
        const unsigned E = w == 16 ? 5 : w == 32 ? 8 : 11, F = w - E - 1;
        const u64 sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
        const u64 exp = ((b6 ^ 1) << (E - 1)) | ((b6 ? ((1ull << (E - 3)) - 1) : 0) << 2) | ((imm8 >> 4) & 3);
        put(rd, (sign << (w - 1)) | (exp << F) | (u64(imm8 & 0xF) << (F - 4)));
        return true;
    }

    const u32 op2 = Bits(instr, 10, 2);
    // --- FCCMP / FCCMPE: compara si se cumple la condicion; si no, NZCV = #nzcv ---
    if (op2 == 0b01) {
        if (ConditionHolds(Bits(instr, 12, 4))) set_nzcv(FP::Compare(get(rn), get(rm), w, Bit(instr, 4), env));
        else                                    set_nzcv(Bits(instr, 0, 4));
        return true;
    }

    // --- 2 operandos: FMUL, FDIV, FADD, FSUB, FMAX, FMIN, FMAXNM, FMINNM, FNMUL ---
    if (op2 == 0b10) {
        const u64 a = get(rn), b = get(rm);
        switch (Bits(instr, 12, 4)) {
            case 0b0000: put(rd, FP::Mul(a, b, w, env)); return true;
            case 0b0001: put(rd, FP::Div(a, b, w, env)); return true;
            case 0b0010: put(rd, FP::Add(a, b, w, env)); return true;
            case 0b0011: put(rd, FP::Sub(a, b, w, env)); return true;
            case 0b0100: put(rd, FP::Max(a, b, w, env)); return true;
            case 0b0101: put(rd, FP::Min(a, b, w, env)); return true;
            case 0b0110: put(rd, FP::MaxNum(a, b, w, env)); return true;
            case 0b0111: put(rd, FP::MinNum(a, b, w, env)); return true;
            case 0b1000: put(rd, FP::Neg(FP::Mul(a, b, w, env), w)); return true;     // FNMUL
            default: return false;
        }
    }

    // --- FCSEL: Vd = condicion ? Vn : Vm ---
    put(rd, ConditionHolds(Bits(instr, 12, 4)) ? get(rn) : get(rm));
    return true;
}

} // namespace NeXo2::Core
