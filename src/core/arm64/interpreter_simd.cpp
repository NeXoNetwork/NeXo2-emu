// SIMD (Advanced SIMD / NEON) vectorial y escalar, por grupos del manual de ARM
// ("Data Processing -- Scalar Floating-Point and Advanced SIMD").
//
//   Vectorial (0 Q U 0111x ...)                 Escalar (01 U 1111x ...)
//     tres iguales (enteros y FP)                 tres iguales
//     tres iguales FP16                           tres iguales FP16
//     extension (SDOT/UDOT, SQRDMLAH)             extension (SQRDMLAH)
//     tres distintos (largos/estrechos)           tres distintos (SQDMULL...)
//     dos registros (+ FP16)                      dos registros (+ FP16)
//     entre carriles (ADDV, FMAXV...)             pares (ADDP, FADDP...)
//     copia (DUP, INS, UMOV, SMOV)                copia (DUP)
//     permutar, EXT, TBL/TBX                      desplazamiento inmediato
//     inmediato (MOVI, MVNI, ORR, BIC, FMOV)      por elemento
//     desplazamiento inmediato
//     por elemento
// La criptografia (AES, SHA) esta en interpreter_crypto.cpp.
//
// Regla general: si una codificacion esta "reservada" en el manual, devolvemos false
// (instruccion no valida), igual que una CPU real. tests/cpu_fuzz_tests.cpp lo comprueba
// contra un ARM de referencia.
#include "simd_common.hpp"

namespace NeXo2::Core {

using namespace Simd;

u64 Simd::ExpandImm(u32 op, u32 cmode, u32 imm8) {
    u64 imm = 0;
    auto rep32 = [](u64 v) { return v | (v << 32); };
    auto rep16 = [](u64 v) { return v | (v << 16) | (v << 32) | (v << 48); };
    switch (cmode >> 1) {
        case 0: imm = rep32(u64(imm8)); break;
        case 1: imm = rep32(u64(imm8) << 8); break;
        case 2: imm = rep32(u64(imm8) << 16); break;
        case 3: imm = rep32(u64(imm8) << 24); break;
        case 4: imm = rep16(u64(imm8)); break;
        case 5: imm = rep16(u64(imm8) << 8); break;
        case 6:
            imm = (cmode & 1) ? rep32((u64(imm8) << 16) | 0xFFFF) : rep32((u64(imm8) << 8) | 0xFF);
            break;
        default:
            if ((cmode & 1) == 0 && op == 0) {           // MOVI .16b: el byte repetido
                imm = u64(imm8) * 0x0101010101010101ull;
            } else if ((cmode & 1) == 0 && op == 1) {    // MOVI .2d: cada bit -> un byte
                for (int i = 0; i < 8; ++i) if ((imm8 >> i) & 1) imm |= 0xFFull << (i * 8);
            } else if ((cmode & 1) == 1 && op == 0) {    // FMOV single
                const u64 s = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
                const u64 f = (s << 31) | ((b ^ 1) << 30) | ((b ? 0x1Full : 0) << 25) | (u64(imm8 & 0x3F) << 19);
                imm = rep32(f);
            } else {                                      // FMOV double
                const u64 s = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
                imm = (s << 63) | ((b ^ 1) << 62) | ((b ? 0xFFull : 0) << 54) | (u64(imm8 & 0x3F) << 48);
            }
            break;
    }
    return imm;
}

struct SimdOps {
    using V = V128;
    static V& R(Interpreter& it, unsigned n) { return it.m_state.v[n & 31]; }
    static void Write(Interpreter& it, unsigned rd, V v, bool q) {
        if (!q) v.hi = 0;
        R(it, rd) = v;
    }
    // Escribe un escalar de 'eb' bytes (el resto del registro a cero)
    static void WriteScalar(Interpreter& it, unsigned rd, u64 v, unsigned eb) {
        V r{};
        r.Set(0, eb, v & LaneMask(eb));
        R(it, rd) = r;
    }
    static void QC(Interpreter& it) { it.m_state.fpsr |= FP::FPSR_QC; }
    static FP::Env Env(Interpreter& it) { return FP::Env{u32(it.m_state.fpcr), it.m_state.fpsr}; }
    static FP::Rounding FpcrRounding(Interpreter& it) { return FP::Rounding(FP::RoundingMode(u32(it.m_state.fpcr))); }

    static bool Dispatch(Interpreter& it, u32 instr);

    // --- grupos vectoriales ---
    static bool ThreeSame(Interpreter& it, u32 instr, bool scalar);
    static bool ThreeSameFP(Interpreter& it, u32 instr, unsigned w, bool scalar, u32 key);
    static bool ThreeSameFP16(Interpreter& it, u32 instr, bool scalar);
    static bool ThreeSameExtra(Interpreter& it, u32 instr, bool scalar);
    static bool ThreeDifferent(Interpreter& it, u32 instr, bool scalar);
    static bool TwoRegMisc(Interpreter& it, u32 instr, bool scalar);
    static bool TwoRegMiscFP(Interpreter& it, u32 instr, unsigned w, bool scalar, u32 key);
    static bool TwoRegMiscFP16(Interpreter& it, u32 instr, bool scalar);
    static bool AcrossLanes(Interpreter& it, u32 instr);
    static bool ScalarPairwise(Interpreter& it, u32 instr);
    static bool Copy(Interpreter& it, u32 instr, bool scalar);
    static bool Permute(Interpreter& it, u32 instr);
    static bool Ext(Interpreter& it, u32 instr);
    static bool Table(Interpreter& it, u32 instr);
    static bool ModifiedImm(Interpreter& it, u32 instr);
    static bool ShiftImm(Interpreter& it, u32 instr, bool scalar);
    static bool ByElement(Interpreter& it, u32 instr, bool scalar);
};

bool Interpreter::ExecSimdVector(u32 instr) { return SimdOps::Dispatch(*this, instr); }

bool SimdOps::Dispatch(Interpreter& it, u32 instr) {
    const bool scalar = Bit(instr, 30) && Bit(instr, 28);       // 01 U 1111x
    if (!scalar && Bit(instr, 31)) return false;
    if (scalar && Bit(instr, 31)) return false;
    const bool b24 = Bit(instr, 24), b21 = Bit(instr, 21), b10 = Bit(instr, 10), b15 = Bit(instr, 15);
    const bool u = Bit(instr, 29);

    if (b24) {                                                  // 0111 1 / 1111 1
        if (!b10) return ByElement(it, instr, scalar);
        if (Bit(instr, 23)) return false;                       // inmediatos: bit 23 = 0
        if (!scalar && Bits(instr, 19, 4) == 0) return ModifiedImm(it, instr);
        if (Bits(instr, 19, 4) == 0) return false;
        return ShiftImm(it, instr, scalar);
    }
    if (b21) {
        if (Bits(instr, 10, 2) == 0b00) return ThreeDifferent(it, instr, scalar);
        if (b10) return ThreeSame(it, instr, scalar);
        // bits 11..10 = 10
        if (Bits(instr, 17, 6) == 0b111100) return TwoRegMiscFP16(it, instr, scalar);   // bits 22..17
        const u32 op = Bits(instr, 17, 4);
        if (op == 0b0000) return TwoRegMisc(it, instr, scalar);
        if (op == 0b1000) return scalar ? ScalarPairwise(it, instr) : AcrossLanes(it, instr);
        if (op == 0b0100 && Bits(instr, 22, 2) == 0 && !u) return it.ExecCrypto(instr);   // AES / SHA 2 reg
        return false;
    }
    // bit 21 = 0
    if (b15 && b10) return ThreeSameExtra(it, instr, scalar);
    if (b10) {
        if (Bits(instr, 21, 3) == 0b000 && !b15) return Copy(it, instr, scalar);
        if (Bits(instr, 21, 2) == 0b10 && Bits(instr, 14, 2) == 0) return ThreeSameFP16(it, instr, scalar);
        return false;
    }
    if (scalar) {
        if (!u && Bits(instr, 21, 3) == 0 && !b15 && Bits(instr, 10, 2) == 0) return it.ExecCrypto(instr);  // SHA 3 reg
        return false;
    }
    if (b15) return false;
    if (u) return (Bits(instr, 22, 2) == 0 && !b10) ? Ext(it, instr) : false;
    if (Bits(instr, 10, 2) == 0b10) return Permute(it, instr);
    if (Bits(instr, 10, 2) == 0b00 && Bits(instr, 22, 2) == 0) return Table(it, instr);
    return false;
}

// ============================================================================
//  Tres iguales (enteros y coma flotante)
// ============================================================================

bool SimdOps::ThreeSame(Interpreter& it, u32 instr, bool scalar) {
    const bool q = Bit(instr, 30) || scalar, u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 11, 5);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);

    if (opcode >= 0b11000) {                                     // coma flotante
        const unsigned w = (size & 1) ? 64 : 32;
        if (!scalar && w == 64 && !q) return false;
        return ThreeSameFP(it, instr, w, scalar, (u32(u) << 4) | ((size >> 1) << 3) | (opcode & 7));
    }

    const unsigned eb = 1u << size;
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / eb;
    const V a = R(it, rn), b = R(it, rm), d = R(it, rd);

    // Logicas: el campo size elige la operacion
    if (opcode == 0b00011) {
        if (scalar) return false;
        V r;
        auto op = [&](u64 x, u64 y, u64 z) -> u64 {
            if (!u) switch (size) { case 0: return x & y; case 1: return x & ~y; case 2: return x | y; default: return x | ~y; }
            switch (size) {
                case 0: return x ^ y;                         // EOR
                case 1: return (z & x) | (~z & y);            // BSL
                case 2: return (y & x) | (~y & z);            // BIT
                default: return (~y & x) | (y & z);           // BIF
            }
        };
        r.lo = op(a.lo, b.lo, d.lo);
        r.hi = op(a.hi, b.hi, d.hi);
        Write(it, rd, r, q);
        return true;
    }

    // Validez de size/Q segun la instruccion
    switch (opcode) {
        case 0b00000: case 0b00010: case 0b00100: case 0b01100: case 0b01101: case 0b01110: case 0b01111:
        case 0b10010: case 0b10100: case 0b10101:
            if (scalar || size == 3) return false;
            break;
        case 0b10011:
            if (scalar || (u ? size != 0 : size == 3)) return false;
            break;
        case 0b10110:
            if (size == 0 || size == 3) return false;
            break;
        case 0b10111:
            if (scalar || u || (size == 3 && !q)) return false;
            break;
        case 0b00110: case 0b00111: case 0b10000: case 0b10001: case 0b01000: case 0b01010:
            if (scalar ? size != 3 : (size == 3 && !q)) return false;
            break;
        default:   // 00001, 00101, 01001, 01011 (saturantes): escalar en cualquier tamano
            if (!scalar && size == 3 && !q) return false;
            break;
    }

    bool sat = false;
    V r{};
    for (unsigned i = 0; i < lanes; ++i) {
        const u64 x = a.Get(i, eb) , y = b.Get(i, eb);
        const s64 sx = Sx(x, eb), sy = Sx(y, eb);
        const I128 wx = Wide(x, eb, u), wy = Wide(y, eb, u);
        u64 z = 0;
        switch (opcode) {
            case 0b00000: z = u ? (x + y) >> 1 : u64((sx + sy) >> 1); break;                        // HADD
            case 0b00001: z = u ? SatU(wx + wy, eb, sat) : SatS(wx + wy, eb, sat); break;          // QADD
            case 0b00010: z = u ? (x + y + 1) >> 1 : u64((sx + sy + 1) >> 1); break;               // RHADD
            case 0b00100: z = u64((u ? s64(x) - s64(y) : sx - sy) >> 1); break;                    // HSUB
            case 0b00101: z = u ? SatU(wx - wy, eb, sat) : SatS(wx - wy, eb, sat); break;          // QSUB
            case 0b00110: z = (u ? x > y : sx > sy) ? ~0ull : 0; break;                            // CMHI / CMGT
            case 0b00111: z = (u ? x >= y : sx >= sy) ? ~0ull : 0; break;                          // CMHS / CMGE
            case 0b01000: z = ShiftByReg(x, eb, s8(y & 0xFF), u, false, false, sat); break;        // USHL / SSHL
            case 0b01001: z = ShiftByReg(x, eb, s8(y & 0xFF), u, false, true, sat); break;         // QSHL
            case 0b01010: z = ShiftByReg(x, eb, s8(y & 0xFF), u, true, false, sat); break;         // RSHL
            case 0b01011: z = ShiftByReg(x, eb, s8(y & 0xFF), u, true, true, sat); break;          // QRSHL
            case 0b01100: z = u ? (x > y ? x : y) : u64(sx > sy ? sx : sy); break;                 // MAX
            case 0b01101: z = u ? (x < y ? x : y) : u64(sx < sy ? sx : sy); break;                 // MIN
            case 0b01110: z = u ? (x > y ? x - y : y - x) : u64(sx > sy ? sx - sy : sy - sx); break;   // ABD
            case 0b01111: z = d.Get(i, eb) + (u ? (x > y ? x - y : y - x) : u64(sx > sy ? sx - sy : sy - sx)); break; // ABA
            case 0b10000: z = u ? x - y : x + y; break;                                            // SUB / ADD
            case 0b10001: z = (u ? x == y : (x & y) != 0) ? ~0ull : 0; break;                      // CMEQ / CMTST
            case 0b10010: z = u ? d.Get(i, eb) - x * y : d.Get(i, eb) + x * y; break;              // MLS / MLA
            case 0b10011:
                if (!u) { z = x * y; break; }                                                      // MUL
                for (unsigned k = 0; k < 8; ++k) if ((y >> k) & 1) z ^= x << k;                    // PMUL
                break;
            case 0b10100: case 0b10101: case 0b10111: {                                            // pares
                const unsigned half = lanes / 2;
                const V& src = i < half ? a : b;
                const unsigned k = (i % half) * 2;
                const u64 p = src.Get(k, eb), s2 = src.Get(k + 1, eb);
                const s64 sp = Sx(p, eb), ss = Sx(s2, eb);
                if (opcode == 0b10111) z = p + s2;                                                 // ADDP
                else if (opcode == 0b10100) z = u ? (p > s2 ? p : s2) : u64(sp > ss ? sp : ss);    // MAXP
                else z = u ? (p < s2 ? p : s2) : u64(sp < ss ? sp : ss);                           // MINP
                break;
            }
            case 0b10110: {                                                                        // SQ(R)DMULH
                const unsigned bits = eb * 8;
                I128 p = I128::From(sx * sy).Shl(1);
                if (u) p = p + I128::From(s64(1) << (bits - 1));
                z = SatS(p.Sar(bits), eb, sat);
                break;
            }
            default: return false;
        }
        r.Set(i, eb, z & LaneMask(eb));
    }
    if (sat) QC(it);
    if (scalar) WriteScalar(it, rd, r.lo, eb);
    else Write(it, rd, r, q);
    return true;
}

// Una operacion FP de "tres iguales". key = U:a:opcode<2:0>
bool SimdOps::ThreeSameFP(Interpreter& it, u32 instr, unsigned w, bool scalar, u32 key) {
    const bool q = Bit(instr, 30);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    const unsigned eb = w / 8;
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / eb;
    const V a = R(it, rn), b = R(it, rm), d = R(it, rd);
    FP::Env e = Env(it);
    const u64 ones = LaneMask(eb);

    // En escalar solo existen algunas
    if (scalar) {
        switch (key) {
            case 0b00011: case 0b00100: case 0b00111: case 0b01111:          // FMULX FCMEQ FRECPS FRSQRTS
            case 0b10100: case 0b10101: case 0b11010: case 0b11100: case 0b11101: break;   // FCMGE FACGE FABD FCMGT FACGT
            default: return false;
        }
    }
    const bool pairwise = key == 0b10000 || key == 0b10010 || key == 0b10110 || key == 0b11000 || key == 0b11110;
    V r{};
    for (unsigned i = 0; i < lanes; ++i) {
        u64 x = a.Get(i, eb), y = b.Get(i, eb);
        if (pairwise) {
            const unsigned half = lanes / 2;
            const V& src = i < half ? a : b;
            const unsigned k = (i % half) * 2;
            x = src.Get(k, eb);
            y = src.Get(k + 1, eb);
        }
        u64 z;
        switch (key) {
            case 0b00000: case 0b10000: z = FP::MaxNum(x, y, w, e); break;                 // FMAXNM(P)
            case 0b00001: z = FP::MulAdd(d.Get(i, eb), x, y, w, e); break;                 // FMLA
            case 0b00010: z = FP::Add(x, y, w, e); break;                                  // FADD
            case 0b00011: z = FP::MulX(x, y, w, e); break;                                 // FMULX
            case 0b00100: z = FP::CompareEQ(x, y, w, e) ? ones : 0; break;                 // FCMEQ
            case 0b00110: case 0b10110: z = FP::Max(x, y, w, e); break;                    // FMAX(P)
            case 0b00111: z = FP::RecipStepFused(x, y, w, e); break;                       // FRECPS
            case 0b01000: case 0b11000: z = FP::MinNum(x, y, w, e); break;                 // FMINNM(P)
            case 0b01001: z = FP::MulAdd(d.Get(i, eb), FP::Neg(x, w), y, w, e); break;     // FMLS
            case 0b01010: z = FP::Sub(x, y, w, e); break;                                  // FSUB
            case 0b01110: case 0b11110: z = FP::Min(x, y, w, e); break;                    // FMIN(P)
            case 0b01111: z = FP::RSqrtStepFused(x, y, w, e); break;                       // FRSQRTS
            case 0b10010: z = FP::Add(x, y, w, e); break;                                  // FADDP
            case 0b10011: z = FP::Mul(x, y, w, e); break;                                  // FMUL
            case 0b10100: z = FP::CompareGE(x, y, w, e) ? ones : 0; break;                 // FCMGE
            case 0b10101: z = FP::CompareGE(FP::Abs(x, w), FP::Abs(y, w), w, e) ? ones : 0; break;  // FACGE
            case 0b10111: z = FP::Div(x, y, w, e); break;                                  // FDIV
            case 0b11010: z = FP::Abs(FP::Sub(x, y, w, e), w); break;                      // FABD
            case 0b11100: z = FP::CompareGT(x, y, w, e) ? ones : 0; break;                 // FCMGT
            case 0b11101: z = FP::CompareGT(FP::Abs(x, w), FP::Abs(y, w), w, e) ? ones : 0; break;  // FACGT
            default: return false;
        }
        r.Set(i, eb, z);
    }
    if (scalar) WriteScalar(it, rd, r.lo, eb);
    else Write(it, rd, r, q);
    return true;
}

bool SimdOps::ThreeSameFP16(Interpreter& it, u32 instr, bool scalar) {
    const u32 key = (u32(Bit(instr, 29)) << 4) | (u32(Bit(instr, 23)) << 3) | Bits(instr, 11, 3);
    return ThreeSameFP(it, instr, 16, scalar, key);
}

// SQRDMLAH / SQRDMLSH y SDOT / UDOT
bool SimdOps::ThreeSameExtra(Interpreter& it, u32 instr, bool scalar) {
    const bool q = Bit(instr, 30), u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 11, 4);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    const V a = R(it, rn), b = R(it, rm), d = R(it, rd);

    if (opcode == 0b0000 || opcode == 0b0001) {                    // SQRDMLAH / SQRDMLSH
        if (!u || size == 0 || size == 3) return false;
        const unsigned eb = 1u << size, bits = eb * 8;
        const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / eb;
        bool sat = false;
        V r{};
        for (unsigned i = 0; i < lanes; ++i) {
            I128 p = I128::From(Sx(a.Get(i, eb), eb) * Sx(b.Get(i, eb), eb)).Shl(1);
            if (opcode == 1) p = -p;
            const I128 acc = I128::From(Sx(d.Get(i, eb), eb)).Shl(bits);
            const I128 sum = acc + p + I128::From(s64(1) << (bits - 1));
            r.Set(i, eb, SatS(sum.Sar(bits), eb, sat));
        }
        if (sat) QC(it);
        if (scalar) WriteScalar(it, rd, r.lo, eb);
        else Write(it, rd, r, q);
        return true;
    }
    if (opcode == 0b0010 && !scalar) {                              // SDOT / UDOT
        if (size != 2) return false;
        V r = d;
        const unsigned lanes = q ? 4 : 2;
        for (unsigned i = 0; i < lanes; ++i) {
            u64 acc = d.Get(i, 4);
            for (unsigned k = 0; k < 4; ++k) {
                const u64 x = a.Get(i * 4 + k, 1), y = b.Get(i * 4 + k, 1);
                acc += u ? x * y : u64(Sx(x, 1) * Sx(y, 1));
            }
            r.Set(i, 4, acc & 0xFFFFFFFFu);
        }
        Write(it, rd, r, q);
        return true;
    }
    return false;
}

// ============================================================================
//  Tres distintos: operaciones "largas" (resultado del doble de ancho) y "estrechas"
// ============================================================================

bool SimdOps::ThreeDifferent(Interpreter& it, u32 instr, bool scalar) {
    const bool q = Bit(instr, 30), u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 12, 4);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    const V a = R(it, rn), b = R(it, rm), d = R(it, rd);

    if (opcode == 0b1110 && !scalar) {                       // PMULL / PMULL2
        if (u || size == 1 || size == 2) return false;
        V r{};
        if (size == 3) {                                       // 64 x 64 -> 128 (criptografia)
            PolyMul64(q ? a.hi : a.lo, q ? b.hi : b.lo, r.lo, r.hi);
        } else {
            const u64 x = q ? a.hi : a.lo, y = q ? b.hi : b.lo;
            for (unsigned i = 0; i < 8; ++i) {
                const u64 xa = (x >> (i * 8)) & 0xFF, yb = (y >> (i * 8)) & 0xFF;
                u64 p = 0;
                for (unsigned k = 0; k < 8; ++k) if ((yb >> k) & 1) p ^= xa << k;
                r.Set(i, 2, p);
            }
        }
        R(it, rd) = r;
        return true;
    }
    if (size == 3) return false;
    const bool sq_op = opcode == 0b1001 || opcode == 0b1011 || opcode == 0b1101;   // SQDMLAL SQDMLSL SQDMULL
    if (scalar && (!sq_op || u)) return false;
    if (sq_op && (u || size == 0)) return false;

    const unsigned eb = 1u << size, wb = eb * 2;
    const unsigned lanes = scalar ? 1 : 8 / eb;
    const bool upper = q && !scalar;                          // variantes "2": mitad alta
    auto narrow = [&](const V& v, unsigned i) { return upper ? v.Get(i + lanes, eb) : v.Get(i, eb); };
    auto ext = [&](u64 v, unsigned bytes) { return u ? I128::FromU(v & LaneMask(bytes)) : I128::From(Sx(v, bytes)); };

    // Estrechas: ADDHN, RADDHN, SUBHN, RSUBHN (fuentes anchas -> mitad del resultado)
    if (opcode == 0b0100 || opcode == 0b0110) {
        if (scalar) return false;
        V r = upper ? d : V{};
        for (unsigned i = 0; i < lanes; ++i) {
            const u64 x = a.Get(i, wb), y = b.Get(i, wb);
            u64 v = opcode == 0b0100 ? x + y : x - y;
            u64 hi_part = (v >> (eb * 8)) & LaneMask(eb);
            if (u) {   // con redondeo: + 2^(eb*8-1) antes de quedarse con la mitad alta (acarreo incluido)
                const u64 rc = 1ull << (eb * 8 - 1);
                const u64 vw = v & LaneMask(wb);
                const u64 sum = vw + rc;
                hi_part = wb == 8 ? ((sum >> 32) & LaneMask(eb)) : ((sum >> (eb * 8)) & LaneMask(eb));
            }
            r.Set(upper ? i + lanes : i, eb, hi_part);
        }
        Write(it, rd, r, true);
        if (!upper) R(it, rd).hi = 0;
        return true;
    }

    bool sat = false;
    V r{};
    for (unsigned i = 0; i < lanes; ++i) {
        const u64 xn = narrow(a, i), yn = narrow(b, i);
        const I128 x = ext(xn, eb), y = ext(yn, eb);
        I128 z;
        const I128 acc = u ? I128::FromU(d.Get(i, wb)) : I128::From(Sx(d.Get(i, wb), wb));
        switch (opcode) {
            case 0b0000: z = x + y; break;                                              // ADDL
            case 0b0001: z = ext(a.Get(i, wb), wb) + y; break;                         // ADDW
            case 0b0010: z = x - y; break;                                              // SUBL
            case 0b0011: z = ext(a.Get(i, wb), wb) - y; break;                         // SUBW
            case 0b0101: { I128 df = x - y; if (df.hi < 0) df = -df; z = acc + df; break; }  // ABAL
            case 0b0111: { I128 df = x - y; if (df.hi < 0) df = -df; z = df; break; }        // ABDL
            case 0b1000: z = acc + MulS(x.lo, y.lo); if (u) z = acc + I128::FromU(xn * yn); break;   // MLAL
            case 0b1010: z = acc - MulS(x.lo, y.lo); if (u) z = acc - I128::FromU(xn * yn); break;   // MLSL
            case 0b1100: z = MulS(s64(x.lo), s64(y.lo)); if (u) z = I128::FromU(xn * yn); break;     // MULL
            case 0b1001: case 0b1011: case 0b1101: {                                    // SQDMLAL/SQDMLSL/SQDMULL
                const u64 prod = SatS(MulS(Sx(xn, eb), Sx(yn, eb)).Shl(1), wb, sat);
                if (opcode == 0b1101) { z = I128::From(Sx(prod, wb)); break; }
                const I128 p = I128::From(Sx(prod, wb));
                const I128 accs = I128::From(Sx(d.Get(i, wb), wb));
                z = I128::From(Sx(SatS(opcode == 0b1001 ? accs + p : accs - p, wb, sat), wb));
                break;
            }
            default: return false;
        }
        r.Set(i, wb, z.lo & LaneMask(wb));
    }
    if (sat) QC(it);
    if (scalar) WriteScalar(it, rd, r.lo, wb);
    else R(it, rd) = r;
    return true;
}

// ============================================================================
//  Dos registros (miscelanea): REV, CNT, ABS, NEG, comparaciones con 0, XTN...
// ============================================================================

bool SimdOps::TwoRegMisc(Interpreter& it, u32 instr, bool scalar) {
    const bool q = Bit(instr, 30) || scalar, u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 12, 5);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5);

    // Coma flotante (y URECPE/URSQRTE, que estan mezcladas con ellas)
    const bool fp = (opcode >= 0b01100 && opcode <= 0b01111 && (size >> 1)) || opcode == 0b10110 ||
                    opcode == 0b10111 || (opcode >= 0b11000 && opcode != 0b11110);
    if (fp) {
        const unsigned w = (size & 1) ? 64 : 32;
        return TwoRegMiscFP(it, instr, w, scalar, (u32(u) << 6) | ((size >> 1) << 5) | opcode);
    }

    const unsigned eb = 1u << size;
    const unsigned lanes = scalar ? 1 : (Bit(instr, 30) ? 16 : 8) / eb;
    const V a = R(it, rn), d = R(it, rd);
    bool sat = false;
    V r{};

    // Estrechas: XTN, SQXTN, UQXTN, SQXTUN (fuente del doble de ancho)
    if (opcode == 0b10010 || opcode == 0b10100) {
        if (size == 3 || (scalar && opcode == 0b10010 && !u)) return false;
        const unsigned wb = eb * 2;
        const unsigned n = scalar ? 1 : 8 / eb;
        const bool upper = Bit(instr, 30) && !scalar;
        r = upper ? d : V{};
        for (unsigned i = 0; i < n; ++i) {
            const u64 x = a.Get(i, wb);
            u64 z;
            if (opcode == 0b10010 && !u) z = x & LaneMask(eb);                               // XTN
            else if (opcode == 0b10010) z = SatU(Wide(x, wb, false), eb, sat);               // SQXTUN
            else z = u ? SatU(Wide(x, wb, true), eb, sat) : SatS(Wide(x, wb, false), eb, sat); // UQXTN / SQXTN
            r.Set(upper ? i + n : i, eb, z);
        }
        if (sat) QC(it);
        if (scalar) WriteScalar(it, rd, r.lo, eb);
        else { R(it, rd) = r; if (!upper) R(it, rd).hi = 0; }
        return true;
    }
    // SHLL / SHLL2: desplazar a la izquierda el tamano del elemento (largo)
    if (opcode == 0b10011 && u && !scalar) {
        if (size == 3) return false;
        const unsigned n = 8 / eb, wb = eb * 2;
        for (unsigned i = 0; i < n; ++i) {
            const u64 x = Bit(instr, 30) ? a.Get(i + n, eb) : a.Get(i, eb);
            r.Set(i, wb, (x << (eb * 8)) & LaneMask(wb));
        }
        R(it, rd) = r;
        return true;
    }
    // Pares largos: SADDLP, UADDLP, SADALP, UADALP
    if (opcode == 0b00010 || opcode == 0b00110) {
        if (scalar || size == 3) return false;
        const unsigned wb = eb * 2, n = lanes / 2;
        for (unsigned i = 0; i < n; ++i) {
            const u64 x = a.Get(2 * i, eb), y = a.Get(2 * i + 1, eb);
            u64 z = u ? x + y : u64(Sx(x, eb) + Sx(y, eb));
            if (opcode == 0b00110) z += d.Get(i, wb);
            r.Set(i, wb, z & LaneMask(wb));
        }
        Write(it, rd, r, Bit(instr, 30));
        return true;
    }

    // Validez
    switch (opcode) {
        case 0b00000: if (scalar || size == 3 || (u && size >= 2)) return false; break;   // REV64 / REV32
        case 0b00001: if (scalar || u || size != 0) return false; break;                  // REV16
        case 0b00100: if (scalar || size == 3) return false; break;                       // CLS / CLZ
        case 0b00101: if (scalar || (u ? size > 1 : size != 0)) return false; break;      // CNT / NOT / RBIT
        case 0b00011: case 0b00111:                                                        // SUQADD USQADD SQABS SQNEG
            if (!scalar && size == 3 && !q) return false;
            break;
        case 0b01000: case 0b01001: case 0b01011:                                         // CMGT/CMGE/CMEQ/CMLE #0, ABS/NEG
            if (scalar ? size != 3 : (size == 3 && !q)) return false;
            break;
        case 0b01010:                                                                      // CMLT #0
            if (u || (scalar ? size != 3 : (size == 3 && !q))) return false;
            break;
        default: return false;
    }

    if (opcode == 0b00101 && u && size == 1) {                          // RBIT: siempre por bytes
        for (unsigned i = 0; i < (Bit(instr, 30) ? 16u : 8u); ++i) {
            const u64 x = a.Get(i, 1);
            u64 z = 0;
            for (unsigned k = 0; k < 8; ++k) if ((x >> k) & 1) z |= 1ull << (7 - k);
            r.Set(i, 1, z);
        }
        Write(it, rd, r, Bit(instr, 30));
        return true;
    }

    for (unsigned i = 0; i < lanes; ++i) {
        const u64 x = a.Get(i, eb);
        const s64 sx = Sx(x, eb);
        u64 z = 0;
        switch (opcode) {
            case 0b00000: {                                                    // REV64 / REV32
                const unsigned container = (u ? 4u : 8u) / eb;                // elementos por bloque
                const unsigned base = (i / container) * container;
                z = a.Get(base + (container - 1 - (i - base)), eb);
                break;
            }
            case 0b00001: z = a.Get(i ^ 1, 1); break;                          // REV16 (bytes)
            case 0b00011:                                                      // SUQADD / USQADD
                if (!u) z = SatS(Wide(d.Get(i, eb), eb, false) + Wide(x, eb, true), eb, sat);
                else    z = SatU(Wide(d.Get(i, eb), eb, true) + Wide(x, eb, false), eb, sat);
                break;
            case 0b00100: {                                                    // CLS / CLZ
                const unsigned bits = eb * 8;
                unsigned c = 0;
                if (u) { for (int k = int(bits) - 1; k >= 0 && !((x >> k) & 1); --k) ++c; }
                else {
                    const u64 sgn = (x >> (bits - 1)) & 1;
                    for (int k = int(bits) - 2; k >= 0 && ((x >> k) & 1) == sgn; --k) ++c;
                }
                z = c;
                break;
            }
            case 0b00101:
                if (!u) { u64 c = x; c = c - ((c >> 1) & 0x55); c = (c & 0x33) + ((c >> 2) & 0x33); z = (c + (c >> 4)) & 0x0F; } // CNT
                else z = ~x;                                                    // NOT
                break;
            case 0b00111: {                                                    // SQABS / SQNEG
                I128 v = I128::From(sx);
                if (u || sx < 0) v = -v;
                if (!u && sx >= 0) v = I128::From(sx);
                z = SatS(v, eb, sat);
                break;
            }
            case 0b01000: z = (u ? sx >= 0 : sx > 0) ? ~0ull : 0; break;      // CMGE / CMGT #0
            case 0b01001: z = (u ? sx <= 0 : sx == 0) ? ~0ull : 0; break;     // CMLE / CMEQ #0
            case 0b01010: z = sx < 0 ? ~0ull : 0; break;                       // CMLT #0
            case 0b01011: z = u ? 0 - x : (sx < 0 ? 0 - u64(sx) : u64(sx)); break;      // NEG / ABS
            default: return false;
        }
        r.Set(i, eb, z & LaneMask(eb));
    }
    if (sat) QC(it);
    if (scalar) WriteScalar(it, rd, r.lo, eb);
    else Write(it, rd, r, Bit(instr, 30));
    return true;
}

// key = U:a:opcode(5)
bool SimdOps::TwoRegMiscFP(Interpreter& it, u32 instr, unsigned w, bool scalar, u32 key) {
    const bool q = Bit(instr, 30);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5);
    const V a = R(it, rn), d = R(it, rd);
    FP::Env e = Env(it);
    const FP::Rounding fr = FpcrRounding(it);
    const bool sz = Bit(instr, 22);

    // Conversiones entre formatos: FCVTN, FCVTXN (estrechas) y FCVTL (larga)
    if ((key & 0x3F) == 0b0010110 || key == 0b0010111) {   // a = 0; U solo distingue FCVTXN
        const bool to_narrow = key != 0b0010111;
        const bool xn = key == 0b1010110;                     // FCVTXN: redondeo "a impar"
        if (xn && !sz) return false;
        if (to_narrow) {
            const unsigned from = sz ? 64 : 32, to = from / 2;
            if (scalar && !xn) return false;
            const unsigned count = scalar ? 1 : 128 / from;   // 2 (64->32) o 4 (32->16)
            const bool upper = q && !scalar;
            V r = upper ? d : V{};
            for (unsigned i = 0; i < count; ++i) {
                const u64 v = FP::Convert(a.Get(i, from / 8), from, to, xn ? FP::RO : fr, e);
                r.Set(upper ? i + count : i, to / 8, v);
            }
            if (scalar) WriteScalar(it, rd, r.lo, to / 8);
            else { R(it, rd) = r; if (!upper) R(it, rd).hi = 0; }
            return true;
        }
        if (scalar) return false;
        const unsigned from = sz ? 32 : 16, to = from * 2, count = 64 / from;
        V r{};
        for (unsigned i = 0; i < count; ++i) {
            const u64 v = q ? a.Get(i + count, from / 8) : a.Get(i, from / 8);
            r.Set(i, to / 8, FP::Convert(v, from, to, fr, e));
        }
        R(it, rd) = r;
        return true;
    }

    // URECPE / URSQRTE: enteros de 32 bits
    if (key == 0b0111100 || key == 0b1111100) {
        if (scalar || sz) return false;
        V r{};
        const unsigned lanes = q ? 4 : 2;
        for (unsigned i = 0; i < lanes; ++i) {
            const u32 x = u32(a.Get(i, 4));
            r.Set(i, 4, key == 0b0111100 ? FP::URecipEstimate(x) : FP::URSqrtEstimate(x));
        }
        Write(it, rd, r, q);
        return true;
    }

    if (!scalar && w == 64 && !q) return false;
    if (scalar) {
        // En escalar no hay FRINT*, FABS, FNEG ni FSQRT
        switch (key & 0x1F) {
            case 0b11000: case 0b11001: case 0b01111: return false;
            default: break;
        }
        if (key == 0b1111111) return false;    // FSQRT
    }
    const unsigned eb = w / 8;
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / eb;
    const u64 ones = LaneMask(eb);
    V r{};
    for (unsigned i = 0; i < lanes; ++i) {
        const u64 x = a.Get(i, eb);
        u64 z;
        switch (key) {
            case 0b0011000: if (scalar) return false; z = FP::RoundInt(x, w, FP::RN, false, e); break;    // FRINTN
            case 0b0011001: if (scalar) return false; z = FP::RoundInt(x, w, FP::RM, false, e); break;    // FRINTM
            case 0b0111000: if (scalar) return false; z = FP::RoundInt(x, w, FP::RP, false, e); break;    // FRINTP
            case 0b0111001: if (scalar) return false; z = FP::RoundInt(x, w, FP::RZ, false, e); break;    // FRINTZ
            case 0b1011000: if (scalar) return false; z = FP::RoundInt(x, w, FP::RA, false, e); break;    // FRINTA
            case 0b1011001: if (scalar) return false; z = FP::RoundInt(x, w, fr, true, e); break;         // FRINTX
            case 0b1111001: if (scalar) return false; z = FP::RoundInt(x, w, fr, false, e); break;        // FRINTI
            case 0b0011010: z = FP::ToFixed(x, w, 0, false, FP::RN, w, e); break;   // FCVTNS
            case 0b0011011: z = FP::ToFixed(x, w, 0, false, FP::RM, w, e); break;   // FCVTMS
            case 0b0011100: z = FP::ToFixed(x, w, 0, false, FP::RA, w, e); break;   // FCVTAS
            case 0b0111010: z = FP::ToFixed(x, w, 0, false, FP::RP, w, e); break;   // FCVTPS
            case 0b0111011: z = FP::ToFixed(x, w, 0, false, FP::RZ, w, e); break;   // FCVTZS
            case 0b1011010: z = FP::ToFixed(x, w, 0, true, FP::RN, w, e); break;    // FCVTNU
            case 0b1011011: z = FP::ToFixed(x, w, 0, true, FP::RM, w, e); break;    // FCVTMU
            case 0b1011100: z = FP::ToFixed(x, w, 0, true, FP::RA, w, e); break;    // FCVTAU
            case 0b1111010: z = FP::ToFixed(x, w, 0, true, FP::RP, w, e); break;    // FCVTPU
            case 0b1111011: z = FP::ToFixed(x, w, 0, true, FP::RZ, w, e); break;    // FCVTZU
            case 0b0011101: z = FP::FixedToFP(x, w, 0, false, w, fr, e); break;     // SCVTF
            case 0b1011101: z = FP::FixedToFP(x, w, 0, true, w, fr, e); break;      // UCVTF
            case 0b0101100: z = FP::CompareGT(x, 0, w, e) ? ones : 0; break;        // FCMGT #0
            case 0b0101101: z = FP::CompareEQ(x, 0, w, e) ? ones : 0; break;        // FCMEQ #0
            case 0b0101110: z = FP::CompareGT(0, x, w, e) ? ones : 0; break;        // FCMLT #0
            case 0b1101100: z = FP::CompareGE(x, 0, w, e) ? ones : 0; break;        // FCMGE #0
            case 0b1101101: z = FP::CompareGE(0, x, w, e) ? ones : 0; break;        // FCMLE #0
            case 0b0101111: if (scalar) return false; z = FP::Abs(x, w); break;     // FABS
            case 0b1101111: if (scalar) return false; z = FP::Neg(x, w); break;     // FNEG
            case 0b0111101: z = FP::RecipEstimate(x, w, e); break;                  // FRECPE
            case 0b1111101: z = FP::RSqrtEstimate(x, w, e); break;                  // FRSQRTE
            case 0b0111111: if (!scalar) return false; z = FP::RecpX(x, w, e); break;   // FRECPX
            case 0b1111111: if (scalar) return false; z = FP::Sqrt(x, w, e); break;     // FSQRT
            default: return false;
        }
        r.Set(i, eb, z);
    }
    if (scalar) WriteScalar(it, rd, r.lo, eb);
    else Write(it, rd, r, q);
    return true;
}

bool SimdOps::TwoRegMiscFP16(Interpreter& it, u32 instr, bool scalar) {
    const u32 key = (u32(Bit(instr, 29)) << 6) | (u32(Bit(instr, 23)) << 5) | Bits(instr, 12, 5);
    // En FP16 no existen las conversiones de formato ni URECPE/URSQRTE
    switch (key & 0x1F) {
        case 0b10110: case 0b10111: return false;
        default: break;
    }
    if (key == 0b0111100 || key == 0b1111100) return false;
    return TwoRegMiscFP(it, instr, 16, scalar, key);
}

// ============================================================================
//  Entre carriles (ADDV, SMAXV, FMAXV...) y pares escalares (ADDP, FADDP...)
// ============================================================================

namespace {
// Reduce de ARM: mitad baja y mitad alta por separado, y luego juntas (recursivo)
template <typename F>
u64 Reduce(const V128& v, unsigned first, unsigned count, unsigned eb, F op) {
    if (count == 1) return v.Get(first, eb);
    const unsigned half = count / 2;
    return op(Reduce(v, first, half, eb, op), Reduce(v, first + half, half, eb, op));
}
} // namespace

bool SimdOps::AcrossLanes(Interpreter& it, u32 instr) {
    const bool q = Bit(instr, 30), u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 12, 5);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5);
    const V a = R(it, rn);

    if (opcode == 0b01100 || opcode == 0b01111) {               // FMAXNMV FMINNMV FMAXV FMINV
        unsigned w;
        if (u) { if ((size & 1) || !q) return false; w = 32; }
        else   { if (size & 1) return false; w = 16; }
        FP::Env e = Env(it);
        const bool is_min = size >> 1;
        const unsigned eb = w / 8, lanes = (q ? 16 : 8) / eb;
        const u64 r = Reduce(a, 0, lanes, eb, [&](u64 x, u64 y) {
            if (opcode == 0b01100) return is_min ? FP::MinNum(x, y, w, e) : FP::MaxNum(x, y, w, e);
            return is_min ? FP::Min(x, y, w, e) : FP::Max(x, y, w, e);
        });
        WriteScalar(it, rd, r, eb);
        return true;
    }
    if (size == 3 || (size == 2 && !q)) return false;
    const unsigned eb = 1u << size, lanes = (q ? 16 : 8) / eb;
    switch (opcode) {
        case 0b00011: {                                           // SADDLV / UADDLV
            u64 sum = 0;
            for (unsigned i = 0; i < lanes; ++i) sum += u ? a.Get(i, eb) : u64(Sx(a.Get(i, eb), eb));
            WriteScalar(it, rd, sum, eb * 2);
            return true;
        }
        case 0b01010: case 0b11010: {                             // MAXV / MINV
            const bool is_min = opcode == 0b11010;
            u64 best = a.Get(0, eb);
            for (unsigned i = 1; i < lanes; ++i) {
                const u64 x = a.Get(i, eb);
                const bool better = u ? (is_min ? x < best : x > best)
                                      : (is_min ? Sx(x, eb) < Sx(best, eb) : Sx(x, eb) > Sx(best, eb));
                if (better) best = x;
            }
            WriteScalar(it, rd, best, eb);
            return true;
        }
        case 0b11011: {                                           // ADDV
            if (u) return false;
            u64 sum = 0;
            for (unsigned i = 0; i < lanes; ++i) sum += a.Get(i, eb);
            WriteScalar(it, rd, sum, eb);
            return true;
        }
        default: return false;
    }
}

bool SimdOps::ScalarPairwise(Interpreter& it, u32 instr) {
    const bool u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 12, 5);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5);
    const V a = R(it, rn);
    if (opcode == 0b11011) {                                      // ADDP (64 bits)
        if (u || size != 3) return false;
        WriteScalar(it, rd, a.lo + a.hi, 8);
        return true;
    }
    unsigned w;
    if (u) w = (size & 1) ? 64 : 32;
    else { if (size & 1) return false; w = 16; }
    const unsigned eb = w / 8;
    const bool is_min = size >> 1;
    const u64 x = a.Get(0, eb), y = a.Get(1, eb);
    FP::Env e = Env(it);
    u64 z;
    switch (opcode) {
        case 0b01100: z = is_min ? FP::MinNum(x, y, w, e) : FP::MaxNum(x, y, w, e); break;   // FMAXNMP / FMINNMP
        case 0b01101: if (is_min) return false; z = FP::Add(x, y, w, e); break;              // FADDP
        case 0b01111: z = is_min ? FP::Min(x, y, w, e) : FP::Max(x, y, w, e); break;         // FMAXP / FMINP
        default: return false;
    }
    WriteScalar(it, rd, z, eb);
    return true;
}

// ============================================================================
//  Copia, permutar, EXT, TBL/TBX, inmediatos
// ============================================================================

bool SimdOps::Copy(Interpreter& it, u32 instr, bool scalar) {
    const bool q = Bit(instr, 30), op = Bit(instr, 29);
    const u32 imm5 = Bits(instr, 16, 5), imm4 = Bits(instr, 11, 4);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5);
    if ((imm5 & 0xF) == 0) return false;
    unsigned size = 0;
    while (!((imm5 >> size) & 1)) ++size;
    const unsigned eb = 1u << size;
    const unsigned index = imm5 >> (size + 1);

    if (scalar) {                                                  // DUP (escalar) = MOV Bd, Vn.B[i]
        if (op || imm4 != 0) return false;
        WriteScalar(it, rd, R(it, rn).Get(index, eb), eb);
        return true;
    }
    if (op) {                                                      // INS (elemento): MOV Vd.T[i], Vn.T[j]
        if (!q) return false;
        const unsigned index2 = imm4 >> size;
        V r = R(it, rd);
        r.Set(index, eb, R(it, rn).Get(index2, eb));
        R(it, rd) = r;
        return true;
    }
    switch (imm4) {
        case 0b0000: case 0b0001: {                                // DUP (elemento / registro general)
            if (size == 3 && !q) return false;
            const u64 v = imm4 == 0 ? R(it, rn).Get(index, eb) : (it.X(rn) & LaneMask(eb));
            V r{};
            for (unsigned i = 0; i < (q ? 16u : 8u) / eb; ++i) r.Set(i, eb, v);
            Write(it, rd, r, q);
            return true;
        }
        case 0b0011: {                                             // INS (registro general)
            if (!q) return false;
            V r = R(it, rd);
            r.Set(index, eb, it.X(rn) & LaneMask(eb));
            R(it, rd) = r;
            return true;
        }
        case 0b0101: {                                             // SMOV
            if (q ? size > 2 : size > 1) return false;
            it.SetX(rd, u64(Sx(R(it, rn).Get(index, eb), eb)), q);
            return true;
        }
        case 0b0111: {                                             // UMOV
            if (q ? size != 3 : size > 2) return false;
            it.SetX(rd, R(it, rn).Get(index, eb), q);
            return true;
        }
        default: return false;
    }
}

bool SimdOps::Permute(Interpreter& it, u32 instr) {
    const bool q = Bit(instr, 30);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 12, 3);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    if (size == 3 && !q) return false;
    if ((opcode & 3) == 0) return false;
    const unsigned eb = 1u << size, lanes = (q ? 16 : 8) / eb, part = opcode >> 2, half = lanes / 2;
    const V a = R(it, rn), b = R(it, rm);
    V r{};
    for (unsigned i = 0; i < lanes; ++i) {
        u64 v;
        switch (opcode & 3) {
            case 1: {                                               // UZP1 / UZP2
                const unsigned k = 2 * i + part;
                v = k < lanes ? a.Get(k, eb) : b.Get(k - lanes, eb);
                break;
            }
            case 2: {                                               // TRN1 / TRN2
                const unsigned p = i / 2;
                v = (i & 1) ? b.Get(2 * p + part, eb) : a.Get(2 * p + part, eb);
                break;
            }
            default: {                                              // ZIP1 / ZIP2
                const unsigned p = i / 2 + part * half;
                v = (i & 1) ? b.Get(p, eb) : a.Get(p, eb);
                break;
            }
        }
        r.Set(i, eb, v);
    }
    Write(it, rd, r, q);
    return true;
}

bool SimdOps::Ext(Interpreter& it, u32 instr) {
    const bool q = Bit(instr, 30);
    const u32 imm4 = Bits(instr, 11, 4);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    if (!q && (imm4 & 8)) return false;
    const unsigned n = q ? 16 : 8;
    u8 buf[32];
    const V a = R(it, rn), b = R(it, rm);
    std::memcpy(buf, &a, 16);
    std::memcpy(buf + n, &b, 16);
    V r{};
    std::memcpy(&r, buf + imm4, n);
    Write(it, rd, r, q);
    return true;
}

bool SimdOps::Table(Interpreter& it, u32 instr) {
    const bool q = Bit(instr, 30), tbx = Bit(instr, 12);
    const u32 len = Bits(instr, 13, 2) + 1;
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    u8 table[64];
    for (u32 i = 0; i < len; ++i) std::memcpy(table + i * 16, &R(it, (rn + i) % 32), 16);
    const V idx = R(it, rm), d = R(it, rd);
    V r{};
    for (unsigned i = 0; i < (q ? 16u : 8u); ++i) {
        const u64 k = idx.Get(i, 1);
        r.Set(i, 1, k < len * 16 ? table[k] : (tbx ? d.Get(i, 1) : 0));
    }
    Write(it, rd, r, q);
    return true;
}

bool SimdOps::ModifiedImm(Interpreter& it, u32 instr) {
    const bool q = Bit(instr, 30), op = Bit(instr, 29), o2 = Bit(instr, 11);
    const u32 cmode = Bits(instr, 12, 4);
    const u32 imm8 = (Bits(instr, 16, 3) << 5) | Bits(instr, 5, 5);
    const u32 rd = Bits(instr, 0, 5);

    u64 imm;
    if (o2) {                                                      // FMOV (vector, half)
        if (cmode != 0b1111 || op) return false;
        const u64 s = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
        const u64 h = (s << 15) | ((b ^ 1) << 14) | ((b ? 3ull : 0) << 12) | (u64(imm8 & 0x3F) << 6);
        imm = h * 0x0001000100010001ull;
    } else {
        if (cmode == 0b1111 && op && !q) return false;           // FMOV .1d no existe
        imm = ExpandImm(op, cmode, imm8);
    }
    V d = R(it, rd), r{};
    const bool is_orr_bic = !o2 && ((cmode < 8 && (cmode & 1)) || (cmode >= 8 && cmode < 12 && (cmode & 1)));
    const bool is_mvni = !o2 && op && cmode < 14 && !is_orr_bic;
    if (is_orr_bic) {
        r.lo = op ? d.lo & ~imm : d.lo | imm;
        r.hi = op ? d.hi & ~imm : d.hi | imm;
    } else {
        const u64 v = is_mvni ? ~imm : imm;
        r.lo = v;
        r.hi = v;
    }
    Write(it, rd, r, q);
    return true;
}

// ============================================================================
//  Desplazamiento por inmediato (vectorial y escalar)
// ============================================================================

bool SimdOps::ShiftImm(Interpreter& it, u32 instr, bool scalar) {
    const bool q = Bit(instr, 30), u = Bit(instr, 29);
    const u32 immh = Bits(instr, 19, 4), immhb = Bits(instr, 16, 7), opcode = Bits(instr, 11, 5);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5);
    unsigned hs = 3;
    while (!((immh >> hs) & 1)) --hs;
    const unsigned eb = 1u << hs, bits = eb * 8;
    const unsigned rshift = 2 * bits - immhb, lshift = immhb - bits;
    const V a = R(it, rn), d = R(it, rd);
    bool sat = false;

    const bool narrowing = opcode >= 0b10000 && opcode <= 0b10011;
    const bool longop = opcode == 0b10100;
    const bool fixedcvt = opcode == 0b11100 || opcode == 0b11111;

    if (narrowing) {
        if (hs == 3) return false;
        if (scalar && !u && (opcode == 0b10000 || opcode == 0b10001)) return false;   // SHRN/RSHRN no son escalares
        const unsigned wb = eb * 2, n = scalar ? 1 : 8 / eb;
        const bool upper = q && !scalar;
        V r = upper ? d : V{};
        for (unsigned i = 0; i < n; ++i) {
            const u64 x = a.Get(i, wb);
            const bool round = opcode & 1;
            u64 z;
            if (opcode <= 0b10001 && !u) {                          // SHRN / RSHRN
                z = ShiftByReg(x, wb, -s64(rshift), true, round, false, sat) & LaneMask(eb);
            } else if (opcode <= 0b10001) {                         // SQSHRUN / SQRSHRUN
                const u64 v = ShiftByReg(x, wb, -s64(rshift), false, round, false, sat);
                z = SatU(I128::From(Sx(v, wb)), eb, sat);
            } else {                                                // SQSHRN / UQSHRN (+R)
                const u64 v = ShiftByReg(x, wb, -s64(rshift), u, round, false, sat);
                z = u ? SatU(I128::FromU(v), eb, sat) : SatS(I128::From(Sx(v, wb)), eb, sat);
            }
            r.Set(upper ? i + n : i, eb, z);
        }
        if (sat) QC(it);
        if (scalar) WriteScalar(it, rd, r.lo, eb);
        else { R(it, rd) = r; if (!upper) R(it, rd).hi = 0; }
        return true;
    }
    if (longop) {                                                   // SSHLL / USHLL (SXTL, UXTL)
        if (scalar || hs == 3) return false;
        const unsigned n = 8 / eb, wb = eb * 2;
        V r{};
        for (unsigned i = 0; i < n; ++i) {
            const u64 x = q ? a.Get(i + n, eb) : a.Get(i, eb);
            const u64 v = u ? x : u64(Sx(x, eb));
            r.Set(i, wb, (v << lshift) & LaneMask(wb));
        }
        R(it, rd) = r;
        return true;
    }
    if (fixedcvt) {                                                 // SCVTF/UCVTF/FCVTZS/FCVTZU con #fbits
        if (hs == 0) return false;
        if (!scalar && hs == 3 && !q) return false;
        const unsigned w = bits;
        FP::Env e = Env(it);
        const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / eb;
        V r{};
        for (unsigned i = 0; i < lanes; ++i) {
            const u64 x = a.Get(i, eb);
            r.Set(i, eb, opcode == 0b11100 ? FP::FixedToFP(x, w, rshift, u, w, FpcrRounding(it), e)
                                           : FP::ToFixed(x, w, rshift, u, FP::RZ, w, e));
        }
        if (scalar) WriteScalar(it, rd, r.lo, eb);
        else Write(it, rd, r, q);
        return true;
    }

    // Resto: mismo ancho
    const bool sat_left = opcode == 0b01100 || opcode == 0b01110;   // SQSHLU, SQSHL/UQSHL
    if (scalar ? (!sat_left && hs != 3) : (hs == 3 && !q)) return false;
    switch (opcode) {
        case 0b00000: case 0b00010: case 0b00100: case 0b00110: case 0b01010: case 0b01110: break;
        case 0b01000: case 0b01100: if (!u) return false; break;
        default: return false;
    }
    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / eb;
    V r{};
    for (unsigned i = 0; i < lanes; ++i) {
        const u64 x = a.Get(i, eb), dv = d.Get(i, eb);
        u64 z;
        switch (opcode) {
            case 0b00000: z = ShiftByReg(x, eb, -s64(rshift), u, false, false, sat); break;            // SSHR / USHR
            case 0b00010: z = dv + ShiftByReg(x, eb, -s64(rshift), u, false, false, sat); break;       // SSRA / USRA
            case 0b00100: z = ShiftByReg(x, eb, -s64(rshift), u, true, false, sat); break;             // SRSHR / URSHR
            case 0b00110: z = dv + ShiftByReg(x, eb, -s64(rshift), u, true, false, sat); break;        // SRSRA / URSRA
            case 0b01000: {                                                                           // SRI
                const u64 mask = rshift >= bits ? 0 : (LaneMask(eb) >> rshift);
                const u64 sh = rshift >= bits ? 0 : (x >> rshift);
                z = (dv & ~mask) | (sh & mask);
                break;
            }
            case 0b01010:
                if (!u) { z = x << lshift; break; }                                                    // SHL
                z = (dv & ~(LaneMask(eb) << lshift)) | (x << lshift);                                 // SLI
                break;
            case 0b01100:                                                                             // SQSHLU
                z = SatU(Wide(x, eb, false).Shl(lshift), eb, sat);
                break;
            default:                                                                                  // SQSHL / UQSHL #
                z = ShiftByReg(x, eb, s64(lshift), u, false, true, sat);
                break;
        }
        r.Set(i, eb, z & LaneMask(eb));
    }
    if (sat) QC(it);
    if (scalar) WriteScalar(it, rd, r.lo, eb);
    else Write(it, rd, r, q);
    return true;
}

// ============================================================================
//  Por elemento: la segunda fuente es UN elemento de Vm, igual para todos los carriles
// ============================================================================

bool SimdOps::ByElement(Interpreter& it, u32 instr, bool scalar) {
    const bool q = Bit(instr, 30), u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2), opcode = Bits(instr, 12, 4);
    const u32 L = Bit(instr, 21), M = Bit(instr, 20), H = Bit(instr, 11);
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5);
    const V a = R(it, rn), d = R(it, rd);

    // --- Coma flotante: FMLA, FMLS, FMUL, FMULX ---
    const bool fpop = (opcode == 0b0001 || opcode == 0b0101) ? !u : (opcode == 0b1001);
    if (fpop) {
        unsigned w, index;
        u32 rm;
        if (size == 0) { w = 16; index = (H << 2) | (L << 1) | M; rm = Bits(instr, 16, 4); }
        else if (size == 2) { w = 32; index = (H << 1) | L; rm = Bits(instr, 16, 5); }
        else if (size == 3) { if (L) return false; w = 64; index = H; rm = Bits(instr, 16, 5); if (!scalar && !q) return false; }
        else return false;
        const unsigned eb = w / 8, lanes = scalar ? 1 : (q ? 16 : 8) / eb;
        const u64 y = R(it, rm).Get(index, eb);
        FP::Env e = Env(it);
        V r{};
        for (unsigned i = 0; i < lanes; ++i) {
            const u64 x = a.Get(i, eb);
            u64 z;
            switch (opcode) {
                case 0b0001: z = FP::MulAdd(d.Get(i, eb), x, y, w, e); break;                    // FMLA
                case 0b0101: z = FP::MulAdd(d.Get(i, eb), FP::Neg(x, w), y, w, e); break;        // FMLS
                default:     z = u ? FP::MulX(x, y, w, e) : FP::Mul(x, y, w, e); break;           // FMULX / FMUL
            }
            r.Set(i, eb, z);
        }
        if (scalar) WriteScalar(it, rd, r.lo, eb);
        else Write(it, rd, r, q);
        return true;
    }

    // --- Enteros ---
    if (size == 0 || size == 3) return false;
    unsigned index;
    u32 rm;
    if (size == 1) { index = (H << 2) | (L << 1) | M; rm = Bits(instr, 16, 4); }
    else           { index = (H << 1) | L; rm = Bits(instr, 16, 5); }
    const unsigned eb = 1u << size, wb = eb * 2;
    const V b = R(it, rm);

    // SDOT / UDOT (por elemento): 4 bytes de cada carril de 32 bits por los 4 bytes del elemento
    if (opcode == 0b1110) {
        if (scalar || size != 2) return false;
        const unsigned idx = (H << 1) | L;
        const u32 rmd = Bits(instr, 16, 5);
        const V bb = R(it, rmd);
        V r = d;
        for (unsigned i = 0; i < (q ? 4u : 2u); ++i) {
            u64 acc = d.Get(i, 4);
            for (unsigned k = 0; k < 4; ++k) {
                const u64 x = a.Get(i * 4 + k, 1), y = bb.Get(idx * 4 + k, 1);
                acc += u ? x * y : u64(Sx(x, 1) * Sx(y, 1));
            }
            r.Set(i, 4, acc & 0xFFFFFFFFu);
        }
        Write(it, rd, r, q);
        return true;
    }

    const u64 y = b.Get(index, eb);
    const s64 sy = Sx(y, eb);
    bool sat = false;

    const bool longop = opcode == 0b0010 || opcode == 0b0011 || opcode == 0b0110 || opcode == 0b0111 ||
                        opcode == 0b1010 || opcode == 0b1011;
    if (longop) {
        const bool sq = opcode == 0b0011 || opcode == 0b0111 || opcode == 0b1011;
        if (sq && u) return false;
        if (scalar && !sq) return false;
        const unsigned n = scalar ? 1 : 8 / eb;
        const bool upper = q && !scalar;
        V r{};
        for (unsigned i = 0; i < n; ++i) {
            const u64 xn = upper ? a.Get(i + n, eb) : a.Get(i, eb);
            const u64 acc = d.Get(i, wb);
            u64 z;
            if (sq) {
                const u64 prod = SatS(MulS(Sx(xn, eb), sy).Shl(1), wb, sat);
                const I128 p = I128::From(Sx(prod, wb)), ac = I128::From(Sx(acc, wb));
                if (opcode == 0b1011) z = prod;                                             // SQDMULL
                else z = SatS(opcode == 0b0011 ? ac + p : ac - p, wb, sat);                 // SQDMLAL / SQDMLSL
            } else {
                const u64 prod = u ? xn * y : u64(Sx(xn, eb) * sy);
                if (opcode == 0b1010) z = prod;                                             // SMULL / UMULL
                else if (opcode == 0b0010) z = acc + prod;                                  // SMLAL / UMLAL
                else z = acc - prod;                                                        // SMLSL / UMLSL
            }
            r.Set(i, wb, z & LaneMask(wb));
        }
        if (sat) QC(it);
        if (scalar) WriteScalar(it, rd, r.lo, wb);
        else R(it, rd) = r;
        return true;
    }

    const unsigned lanes = scalar ? 1 : (q ? 16 : 8) / eb;
    switch (opcode) {
        case 0b1000: if (u || scalar) return false; break;               // MUL
        case 0b0000: case 0b0100: if (!u || scalar) return false; break; // MLA / MLS
        case 0b1100: if (u) return false; break;                          // SQDMULH
        case 0b1101: break;                                               // SQRDMULH / SQRDMLAH
        case 0b1111: if (!u) return false; break;                         // SQRDMLSH
        default: return false;
    }
    V r{};
    const unsigned bits = eb * 8;
    for (unsigned i = 0; i < lanes; ++i) {
        const u64 x = a.Get(i, eb);
        const s64 sx = Sx(x, eb);
        u64 z;
        switch (opcode) {
            case 0b1000: z = x * y; break;
            case 0b0000: z = d.Get(i, eb) + x * y; break;
            case 0b0100: z = d.Get(i, eb) - x * y; break;
            case 0b1100: case 0b1101:
                if (opcode == 0b1101 && u) {                              // SQRDMLAH
                    const I128 acc = I128::From(Sx(d.Get(i, eb), eb)).Shl(bits);
                    z = SatS((acc + MulS(sx, sy).Shl(1) + I128::From(s64(1) << (bits - 1))).Sar(bits), eb, sat);
                } else {                                                  // SQDMULH / SQRDMULH
                    I128 p = MulS(sx, sy).Shl(1);
                    if (opcode == 0b1101) p = p + I128::From(s64(1) << (bits - 1));
                    z = SatS(p.Sar(bits), eb, sat);
                }
                break;
            default: {                                                    // SQRDMLSH
                const I128 acc = I128::From(Sx(d.Get(i, eb), eb)).Shl(bits);
                z = SatS((acc - MulS(sx, sy).Shl(1) + I128::From(s64(1) << (bits - 1))).Sar(bits), eb, sat);
                break;
            }
        }
        r.Set(i, eb, z & LaneMask(eb));
    }
    if (sat) QC(it);
    if (scalar) WriteScalar(it, rd, r.lo, eb);
    else Write(it, rd, r, q);
    return true;
}

} // namespace NeXo2::Core
