// SIMD vectorial ("Advanced SIMD" / NEON) y su variante escalar.
//
// Un registro v0 de 128 bits se trata como varios "carriles" iguales:
//   v0.16b = 16 x 8 bits   v0.8h = 8 x 16   v0.4s = 4 x 32   v0.2d = 2 x 64
// Con Q = 0 se usan solo los 64 bits bajos (v0.8b, v0.4h, v0.2s) y los altos quedan a 0.
//
// Instrucciones: DUP, INS/MOV (elemento y general), UMOV/SMOV, MOVI/MVNI/ORR/BIC (inmediato),
// FMOV (vector inmediato), AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF, ADD/SUB/MUL/MLA/MLS,
// CMEQ/CMGT/CMGE/CMHI/CMHS/CMTST, SMAX/SMIN/UMAX/UMIN (+ pairwise), ADDP, USHL/SSHL,
// comparaciones con #0, ABS/NEG/NOT/CNT/RBIT/REV, XTN, ADDV/UMAXV/UMINV/SMAXV/SMINV/UADDLV,
// SHL/USHR/SSHR/USRA/SSRA/SHRN/USHLL/SSHLL, UZP/ZIP/TRN, EXT, TBL/TBX,
// FADD/FSUB/FMUL/FDIV/FMLA/FMLS/FMAX/FMIN/FADDP/FCMxx/FABS/FNEG/FSQRT/SCVTF/UCVTF/FCVTZS/FCVTZU.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"
#include <cmath>
#include <cstring>

namespace NeXo2::Core {

using namespace NeXo2::Common;

namespace {

u64 Mask(unsigned bytes) { return Ones(bytes * 8); }
s64 Sx(u64 v, unsigned bytes) { return SignExtend(v, bytes * 8); }

float  F32(u64 b) { float f; u32 x = static_cast<u32>(b); std::memcpy(&f, &x, 4); return f; }
double F64(u64 b) { double d; std::memcpy(&d, &b, 8); return d; }
u64 B32(float f)  { u32 x; std::memcpy(&x, &f, 4); return x; }
u64 B64(double d) { u64 x; std::memcpy(&x, &d, 8); return x; }

// Aplica 'f' carril a carril sobre a y b
template <typename F>
V128 Map2(const V128& a, const V128& b, unsigned bytes, unsigned lanes, F f) {
    V128 r{};
    for (unsigned i = 0; i < lanes; ++i) r.Set(i, bytes, f(a.Get(i, bytes), b.Get(i, bytes)) & Mask(bytes));
    return r;
}

// AdvSIMDExpandImm del manual: el inmediato de MOVI/MVNI/ORR/BIC/FMOV (vector)
u64 ExpandSimdImm(u32 op, u32 cmode, u64 imm8) {
    switch (cmode >> 1) {
        case 0b000: return Replicate(imm8, 32, 64);
        case 0b001: return Replicate(imm8 << 8, 32, 64);
        case 0b010: return Replicate(imm8 << 16, 32, 64);
        case 0b011: return Replicate(imm8 << 24, 32, 64);
        case 0b100: return Replicate(imm8, 16, 64);
        case 0b101: return Replicate(imm8 << 8, 16, 64);
        case 0b110: return (cmode & 1) ? Replicate((imm8 << 16) | 0xFFFF, 32, 64)  // MSL #16
                                       : Replicate((imm8 << 8) | 0xFF, 32, 64);    // MSL #8
        default: break;
    }
    if ((cmode & 1) == 0) {
        if (op == 0) return Replicate(imm8, 8, 64);                // MOVI 8 bits
        u64 r = 0;                                                  // MOVI 64 bits: cada bit -> 1 byte
        for (int i = 0; i < 8; ++i) if ((imm8 >> i) & 1) r |= 0xFFULL << (i * 8);
        return r;
    }
    // FMOV (vector, inmediato)
    const u64 a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cdefgh = imm8 & 0x3F;
    if (op == 0) {
        const u64 f32 = (a << 31) | ((b ^ 1) << 30) | ((b ? 0x1FULL : 0) << 25) | (cdefgh << 19);
        return Replicate(f32, 32, 64);
    }
    return (a << 63) | ((b ^ 1) << 62) | ((b ? 0xFFULL : 0) << 54) | (cdefgh << 48);
}

} // namespace

bool Interpreter::ExecSimdVector(u32 instr) {
    const u32 rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);
    const bool q = Bit(instr, 30);
    const bool u = Bit(instr, 29);
    const u32 size = Bits(instr, 22, 2);
    const unsigned total = q ? 16 : 8;            // bytes usados del registro

    // Escribe el resultado respetando Q (con Q = 0 los 64 bits altos van a cero)
    auto write = [&](const V128& r) {
        V128 out = r;
        if (!q) out.hi = 0;
        Vreg(rd) = out;
    };

    // ====================================================================
    //  SIMD escalar (bits 31..30 = 01, 28..24 = 11110): opera con d0, s0...
    // ====================================================================
    if (Bits(instr, 30, 2) == 0b01 && Bits(instr, 24, 5) == 0b11110) {
        const unsigned bytes = 1u << size;
        const u64 a = Vreg(rn).Get(0, 8), b = Vreg(rm).Get(0, 8);

        // DUP (elemento) escalar: "mov d0, v1.d[1]"
        if (!u && Bits(instr, 21, 3) == 0b000 && Bits(instr, 10, 6) == 0b000001) {
            const u32 imm5 = Bits(instr, 16, 5);
            unsigned sz = 0;
            while (sz < 4 && !((imm5 >> sz) & 1)) ++sz;
            if (sz > 3) return false;
            const unsigned eb = 1u << sz;
            SetVScalar(rd, Vreg(rn).Get(imm5 >> (sz + 1), eb), eb);
            return true;
        }
        // Dos registros (misc): CMxx #0, ABS, NEG, SCVTF/UCVTF, FCVTZS/FCVTZU...
        if (Bits(instr, 17, 5) == 0b10000 && Bits(instr, 10, 2) == 0b10) {
            const u32 opcode = Bits(instr, 12, 5);
            if (opcode >= 0b01000 && opcode <= 0b01011 && size == 3) {
                const s64 v = static_cast<s64>(a);
                u64 r;
                if (opcode == 0b01000)      r = (u ? v >= 0 : v > 0) ? ~0ULL : 0;  // CMGE / CMGT #0
                else if (opcode == 0b01001) r = (u ? v <= 0 : v == 0) ? ~0ULL : 0; // CMLE / CMEQ #0
                else if (opcode == 0b01010) { if (u) return false; r = v < 0 ? ~0ULL : 0; } // CMLT #0
                else                        r = u ? 0 - a : static_cast<u64>(v < 0 ? -v : v); // NEG / ABS
                SetVScalar(rd, r, 8);
                return true;
            }
            const bool fp_double = size & 1;
            const unsigned fb = fp_double ? 8 : 4;
            if (opcode == 0b11101 && (size >> 1) == 0) {                        // SCVTF / UCVTF
                const u64 raw = Vreg(rn).Get(0, fb);
                if (fp_double) SetVScalar(rd, B64(u ? double(raw) : double(Sx(raw, 8))), 8);
                else           SetVScalar(rd, B32(u ? float(u32(raw)) : float(s32(raw))), 4);
                return true;
            }
            if (opcode == 0b11011 && (size >> 1) == 1) {                        // FCVTZS / FCVTZU
                const double v = fp_double ? F64(a) : double(F32(a));
                const unsigned ib = fb * 8;
                u64 r = 0;
                if (!std::isnan(v)) {
                    const double t = std::trunc(v);
                    if (!u) {
                        const double max = std::ldexp(1.0, int(ib) - 1);
                        r = t >= max ? Ones(ib - 1) : t < -max ? (1ULL << (ib - 1)) : u64(s64(t)) & Ones(ib);
                    } else {
                        r = t <= 0 ? 0 : t >= std::ldexp(1.0, int(ib)) ? Ones(ib) : u64(t);
                    }
                }
                SetVScalar(rd, r, fb);
                return true;
            }
            return false;
        }
        // Tres iguales escalar (solo 64 bits): ADD, SUB, CMEQ, CMGT, CMGE, CMHI, CMHS, CMTST
        if (Bit(instr, 21) && Bit(instr, 10) && size == 3) {
            const u32 opcode = Bits(instr, 11, 5);
            const s64 sa = static_cast<s64>(a), sb = static_cast<s64>(b);
            u64 r;
            switch (opcode) {
                case 0b10000: r = u ? a - b : a + b; break;
                case 0b10001: r = (u ? a == b : (a & b) != 0) ? ~0ULL : 0; break;
                case 0b00110: r = (u ? a > b : sa > sb) ? ~0ULL : 0; break;
                case 0b00111: r = (u ? a >= b : sa >= sb) ? ~0ULL : 0; break;
                default: return false;
            }
            SetVScalar(rd, r, 8);
            return true;
        }
        // ADDP escalar: "addp d0, v1.2d"
        if (!u && Bits(instr, 17, 5) == 0b11000 && Bits(instr, 12, 5) == 0b11011 &&
            Bits(instr, 10, 2) == 0b10 && size == 3) {
            SetVScalar(rd, Vreg(rn).lo + Vreg(rn).hi, 8);
            return true;
        }
        (void)bytes;
        return false;
    }
    // Desplazamiento por inmediato escalar (01 U 111110 immh immb opcode 1): SHL/USHR/SSHR d
    if (Bits(instr, 30, 2) == 0b01 && Bits(instr, 23, 6) == 0b111110 && Bit(instr, 10)) {
        const u32 immh = Bits(instr, 19, 4), immhb = Bits(instr, 16, 7);
        if (!(immh & 0b1000)) return false;      // solo 64 bits
        const u32 opcode = Bits(instr, 11, 5);
        const u64 a = Vreg(rn).lo;
        if (opcode == 0b01010 && !u) { SetVScalar(rd, a << (immhb - 64), 8); return true; } // SHL
        if (opcode == 0b00000) {                                                              // USHR / SSHR
            const unsigned sh = 128 - immhb;
            const u64 r = u ? (sh >= 64 ? 0 : a >> sh) : u64(static_cast<s64>(a) >> (sh >= 64 ? 63 : sh));
            SetVScalar(rd, r, 8);
            return true;
        }
        return false;
    }

    if (Bit(instr, 31) != 0) return false;

    // ====================================================================
    //  Inmediato modificado: MOVI, MVNI, ORR, BIC, FMOV (vector)
    //  0 Q op 0111100000 abc cmode o2 1 defgh Rd
    // ====================================================================
    if (Bits(instr, 19, 10) == 0b0111100000 && Bit(instr, 10)) {
        const u32 cmode = Bits(instr, 12, 4);
        const u64 imm8 = (u64(Bits(instr, 16, 3)) << 5) | Bits(instr, 5, 5);
        const bool op = u;
        if (op && cmode == 0b1111 && !q) return false;
        const u64 imm = ExpandSimdImm(op, cmode, imm8);
        V128 r{imm, imm};
        const bool is_orr_bic = (cmode & 1) && (cmode >> 2) != 0b11 && (cmode >> 1) != 0b110;
        if (is_orr_bic) {                        // ORR / BIC (inmediato) modifican Vd
            V128 cur = Vreg(rd);
            if (op) { cur.lo &= ~imm; cur.hi &= ~imm; }   // BIC
            else    { cur.lo |= imm;  cur.hi |= imm;  }   // ORR
            write(cur);
            return true;
        }
        const bool is_mvni = op && cmode != 0b1110 && cmode != 0b1111;
        if (is_mvni) { r.lo = ~imm; r.hi = ~imm; }
        write(r);
        return true;
    }

    // ====================================================================
    //  Desplazamiento por inmediato (vector): 0 Q U 011110 immh immb opcode 1
    // ====================================================================
    if (Bits(instr, 23, 6) == 0b011110 && Bit(instr, 10) && Bits(instr, 19, 4) != 0) {
        const u32 immh = Bits(instr, 19, 4), immhb = Bits(instr, 16, 7), opcode = Bits(instr, 11, 5);
        // Tamano del carril segun el bit mas alto de immh: 0001 -> 8, 001x -> 16, 01xx -> 32, 1xxx -> 64
        unsigned esize;
        if (immh & 0b1000) esize = 64; else if (immh & 0b0100) esize = 32; else if (immh & 0b0010) esize = 16; else esize = 8;
        const unsigned eb = esize / 8;
        const V128 a = Vreg(rn);

        switch (opcode) {
            case 0b00000: case 0b00010: {                       // USHR/SSHR (+ USRA/SSRA)
                if (esize == 64 && !q) return false;
                const unsigned sh = 2 * esize - immhb;
                const bool acc = (opcode == 0b00010);
                V128 r = acc ? Vreg(rd) : V128{};
                for (unsigned i = 0; i < total / eb; ++i) {
                    const u64 v = a.Get(i, eb);
                    const u64 s = u ? (sh >= esize ? 0 : v >> sh)
                                    : u64(Sx(v, eb) >> (sh >= esize ? esize - 1 : sh));
                    r.Set(i, eb, ((acc ? r.Get(i, eb) : 0) + s) & Mask(eb));
                }
                write(r);
                return true;
            }
            case 0b01010: {                                     // SHL
                if (u) return false;                            // SLI: pendiente
                const unsigned sh = immhb - esize;
                write(Map2(a, a, eb, total / eb, [&](u64 v, u64) { return v << sh; }));
                return true;
            }
            case 0b10000: {                                     // SHRN / SHRN2 (estrecha a la mitad)
                if (u || esize == 64) return false;
                const unsigned db = eb, sb = eb * 2;            // destino / origen
                const unsigned sh = 2 * esize - immhb;
                V128 r = q ? Vreg(rd) : V128{};
                const unsigned base = q ? 8 / db : 0;
                for (unsigned i = 0; i < 8 / db; ++i) r.Set(base + i, db, (a.Get(i, sb) >> sh) & Mask(db));
                Vreg(rd) = r;                                   // aqui Q elige mitad, no borra
                if (!q) Vreg(rd).hi = 0;
                return true;
            }
            case 0b10100: {                                     // USHLL/SSHLL (UXTL/SXTL)
                if (esize == 64) return false;
                const unsigned sb = eb, db = eb * 2;
                const unsigned sh = immhb - esize;
                const unsigned base = q ? 8 / sb : 0;           // USHLL2 usa la mitad alta
                V128 r{};
                for (unsigned i = 0; i < 8 / sb; ++i) {
                    const u64 v = a.Get(base + i, sb);
                    r.Set(i, db, ((u ? v : u64(Sx(v, sb))) << sh) & Mask(db));
                }
                Vreg(rd) = r;
                return true;
            }
            default: return false;
        }
    }

    // ====================================================================
    //  EXT: concatena Vm:Vn y extrae desde el byte imm4
    // ====================================================================
    if (u && Bits(instr, 24, 5) == 0b01110 && Bits(instr, 21, 3) == 0 && !Bit(instr, 15) && !Bit(instr, 10)) {
        const u32 imm4 = Bits(instr, 11, 4);
        if (!q && imm4 >= 8) return false;
        u8 buf[32];
        std::memcpy(buf, &Vreg(rn), total);
        std::memcpy(buf + total, &Vreg(rm), total);
        V128 r{};
        std::memcpy(&r, buf + imm4, total);
        write(r);
        return true;
    }

    // ====================================================================
    //  TBL / TBX: busqueda de bytes en una tabla de 1 a 4 registros
    // ====================================================================
    if (!u && Bits(instr, 24, 5) == 0b01110 && Bits(instr, 21, 3) == 0 && Bits(instr, 10, 2) == 0 && !Bit(instr, 15)) {
        const unsigned len = Bits(instr, 13, 2) + 1;
        const bool tbx = Bit(instr, 12);
        u8 table[64];
        for (unsigned i = 0; i < len; ++i) std::memcpy(table + 16 * i, &Vreg((rn + i) % 32), 16);
        const V128 idx = Vreg(rm);
        V128 r = tbx ? Vreg(rd) : V128{};
        for (unsigned i = 0; i < total; ++i) {
            const u64 k = idx.Get(i, 1);
            if (k < 16 * len) r.Set(i, 1, table[k]);
        }
        write(r);
        return true;
    }

    if (Bits(instr, 24, 5) != 0b01110) return false; // por elemento (FMUL v.s[i]...): pendiente

    // ====================================================================
    //  Copia: DUP, INS (MOV v.s[1], w0), UMOV/SMOV (MOV x0, v.d[0])
    //  0 Q op 01110000 imm5 0 imm4 1 Rn Rd
    // ====================================================================
    if (Bits(instr, 21, 3) == 0 && !Bit(instr, 15) && Bit(instr, 10)) {
        const u32 imm5 = Bits(instr, 16, 5), imm4 = Bits(instr, 11, 4);
        unsigned sz = 0;
        while (sz < 4 && !((imm5 >> sz) & 1)) ++sz;
        if (sz > 3) return false;
        const unsigned eb = 1u << sz;
        const unsigned index = imm5 >> (sz + 1);

        if (u) {                                         // INS (elemento): mov v0.b[1], v1.b[0]
            Vreg(rd).Set(index, eb, Vreg(rn).Get(imm4 >> sz, eb));
            return true;
        }
        switch (imm4) {
            case 0b0000: {                               // DUP (elemento)
                const u64 v = Vreg(rn).Get(index, eb);
                V128 r{};
                for (unsigned i = 0; i < total / eb; ++i) r.Set(i, eb, v);
                write(r);
                return true;
            }
            case 0b0001: {                               // DUP (general): dup v0.16b, w1
                const u64 v = X(rn) & Mask(eb);
                V128 r{};
                for (unsigned i = 0; i < total / eb; ++i) r.Set(i, eb, v);
                write(r);
                return true;
            }
            case 0b0011:                                 // INS (general): mov v0.s[1], w1
                Vreg(rd).Set(index, eb, X(rn) & Mask(eb));
                return true;
            case 0b0101:                                 // SMOV
                SetX(rd, u64(Sx(Vreg(rn).Get(index, eb), eb)), q);
                return true;
            case 0b0111:                                 // UMOV: mov x0, v1.d[0]
                SetX(rd, Vreg(rn).Get(index, eb), q);
                return true;
            default: return false;
        }
    }

    // ====================================================================
    //  Permutaciones: UZP1/2, TRN1/2, ZIP1/2
    // ====================================================================
    if (!u && !Bit(instr, 21) && !Bit(instr, 15) && Bits(instr, 10, 2) == 0b10) {
        const unsigned eb = 1u << size, lanes = total / eb, half = lanes / 2;
        const V128 n = Vreg(rn), m = Vreg(rm);
        auto cat = [&](unsigned k) { return k < lanes ? n.Get(k, eb) : m.Get(k - lanes, eb); };
        const u32 opcode = Bits(instr, 12, 3);
        V128 r{};
        for (unsigned i = 0; i < lanes; ++i) {
            u64 v;
            switch (opcode) {
                case 0b001: v = cat(2 * i);     break;                                  // UZP1
                case 0b101: v = cat(2 * i + 1); break;                                  // UZP2
                case 0b011: v = (i & 1) ? m.Get(i / 2, eb) : n.Get(i / 2, eb); break;   // ZIP1
                case 0b111: v = (i & 1) ? m.Get(half + i / 2, eb) : n.Get(half + i / 2, eb); break; // ZIP2
                case 0b010: v = (i & 1) ? m.Get(i - 1, eb) : n.Get(i, eb); break;       // TRN1
                case 0b110: v = (i & 1) ? m.Get(i, eb) : n.Get(i + 1, eb); break;       // TRN2
                default: return false;
            }
            r.Set(i, eb, v);
        }
        write(r);
        return true;
    }

    // ====================================================================
    //  Dos registros (misc): 0 Q U 01110 size 10000 opcode 10 Rn Rd
    // ====================================================================
    if (Bits(instr, 17, 5) == 0b10000 && Bits(instr, 10, 2) == 0b10) {
        const u32 opcode = Bits(instr, 12, 5);
        const unsigned eb = 1u << size, lanes = total / eb;
        const V128 a = Vreg(rn);
        V128 r{};
        auto each = [&](auto f) { for (unsigned i = 0; i < lanes; ++i) r.Set(i, eb, f(a.Get(i, eb)) & Mask(eb)); };
        switch (opcode) {
            case 0b01000: each([&](u64 v) { return (u ? Sx(v, eb) >= 0 : Sx(v, eb) > 0) ? ~0ULL : 0; }); break;  // CMGE/CMGT #0
            case 0b01001: each([&](u64 v) { return (u ? Sx(v, eb) <= 0 : v == 0) ? ~0ULL : 0; }); break;        // CMLE/CMEQ #0
            case 0b01010: if (u) return false; each([&](u64 v) { return Sx(v, eb) < 0 ? ~0ULL : 0; }); break;  // CMLT #0
            case 0b01011: each([&](u64 v) { const s64 s = Sx(v, eb); return u ? u64(0 - v) : u64(s < 0 ? -s : s); }); break; // NEG/ABS
            case 0b00101: {   // NOT / CNT / RBIT: siempre por bytes; 'size' elige la operacion
                for (unsigned i = 0; i < total; ++i) {
                    const u64 v = a.Get(i, 1);
                    u64 o = 0;
                    if (size == 0 && u)       o = ~v & 0xFF;                                               // NOT
                    else if (size == 0 && !u) { u64 x = v; while (x) { o += x & 1; x >>= 1; } }          // CNT
                    else if (size == 1 && u)  { for (int k = 0; k < 8; ++k) if ((v >> k) & 1) o |= 1ULL << (7 - k); } // RBIT
                    else return false;
                    r.Set(i, 1, o);
                }
                break;
            }
            case 0b00000: {                                                     // REV64 / REV32
                const unsigned container = u ? 4 : 8;
                if (eb >= container) return false;
                for (unsigned i = 0; i < lanes; ++i) {
                    const unsigned per = container / eb, blk = i / per, k = i % per;
                    r.Set(i, eb, a.Get(blk * per + (per - 1 - k), eb));
                }
                break;
            }
            case 0b00001: {                                                     // REV16
                if (u || eb != 1) return false;
                for (unsigned i = 0; i < lanes; ++i) r.Set(i, 1, a.Get(i ^ 1, 1));
                break;
            }
            case 0b10010: {                                                     // XTN / XTN2
                if (u || size == 3) return false;
                const unsigned db = eb, sb = eb * 2;
                V128 out = q ? Vreg(rd) : V128{};
                const unsigned base = q ? 8 / db : 0;
                for (unsigned i = 0; i < 8 / db; ++i) out.Set(base + i, db, a.Get(i, sb) & Mask(db));
                Vreg(rd) = out;
                return true;
            }
            default: {
                // Coma flotante vectorial: size<0> = 0 float, 1 double; size<1> distingue la operacion
                const bool fd = size & 1;
                const unsigned fb = fd ? 8 : 4, fl = total / fb;
                auto fget = [&](u64 v) { return fd ? F64(v) : double(F32(v)); };
                auto fput = [&](double d) { return fd ? B64(d) : B32(float(d)); };
                auto fmap = [&](auto f) { for (unsigned i = 0; i < fl; ++i) r.Set(i, fb, f(a.Get(i, fb))); };
                const u64 sign = fd ? (1ULL << 63) : (1ULL << 31);
                if (opcode == 0b01111 && (size >> 1)) { fmap([&](u64 v) { return u ? v ^ sign : v & ~sign; }); break; } // FNEG/FABS
                if (opcode == 0b11111 && (size >> 1) && u) { fmap([&](u64 v) { return fput(std::sqrt(fget(v))); }); break; } // FSQRT
                if (opcode == 0b11101 && !(size >> 1)) {                                                           // SCVTF/UCVTF
                    fmap([&](u64 v) { return fd ? B64(u ? double(v) : double(s64(v))) : B32(u ? float(u32(v)) : float(s32(v))); });
                    break;
                }
                if (opcode == 0b11011 && (size >> 1)) {                                                            // FCVTZS/FCVTZU
                    const unsigned ib = fb * 8;
                    fmap([&](u64 v) -> u64 {
                        const double d = std::trunc(fget(v));
                        if (std::isnan(d)) return 0;
                        if (!u) { const double mx = std::ldexp(1.0, int(ib) - 1);
                                  return d >= mx ? Ones(ib - 1) : d < -mx ? (1ULL << (ib - 1)) : u64(s64(d)) & Ones(ib); }
                        return d <= 0 ? 0 : d >= std::ldexp(1.0, int(ib)) ? Ones(ib) : u64(d);
                    });
                    break;
                }
                return false;
            }
        }
        write(r);
        return true;
    }

    // ====================================================================
    //  Entre carriles: ADDV, UMAXV/SMAXV, UMINV/SMINV, UADDLV/SADDLV
    // ====================================================================
    if (Bits(instr, 17, 5) == 0b11000 && Bits(instr, 10, 2) == 0b10) {
        const u32 opcode = Bits(instr, 12, 5);
        const unsigned eb = 1u << size, lanes = total / eb;
        if (size == 3) return false;
        const V128 a = Vreg(rn);
        u64 acc = a.Get(0, eb);
        s64 sacc = Sx(acc, eb);
        for (unsigned i = 1; i < lanes; ++i) {
            const u64 v = a.Get(i, eb);
            const s64 s = Sx(v, eb);
            switch (opcode) {
                case 0b11011: acc += v; break;                                        // ADDV
                case 0b00011: if (u) acc += v; else sacc += s; break;                 // UADDLV/SADDLV
                case 0b01010: if (u) { if (v > acc) acc = v; } else if (s > sacc) sacc = s; break; // UMAXV/SMAXV
                case 0b11010: if (u) { if (v < acc) acc = v; } else if (s < sacc) sacc = s; break; // UMINV/SMINV
                default: return false;
            }
        }
        if (opcode == 0b11011 && u) return false;
        if (opcode == 0b00011) SetVScalar(rd, (u ? acc : u64(sacc)) & Mask(eb * 2), eb * 2);
        else if (!u && opcode != 0b11011) SetVScalar(rd, u64(sacc) & Mask(eb), eb);
        else SetVScalar(rd, acc & Mask(eb), eb);
        return true;
    }

    // ====================================================================
    //  Tres iguales: 0 Q U 01110 size 1 Rm opcode 1 Rn Rd
    // ====================================================================
    if (Bit(instr, 21) && Bit(instr, 10)) {
        const u32 opcode = Bits(instr, 11, 5);
        const unsigned eb = 1u << size, lanes = total / eb;
        const V128 a = Vreg(rn), b = Vreg(rm);

        // --- Logicas (size elige la operacion) ---
        if (opcode == 0b00011) {
            V128 r{};
            const V128 d = Vreg(rd);
            auto bitwise = [&](auto f) { r.lo = f(a.lo, b.lo, d.lo); r.hi = f(a.hi, b.hi, d.hi); };
            switch ((u ? 4 : 0) | size) {
                case 0: bitwise([](u64 x, u64 y, u64) { return x & y; });  break;           // AND
                case 1: bitwise([](u64 x, u64 y, u64) { return x & ~y; }); break;           // BIC
                case 2: bitwise([](u64 x, u64 y, u64) { return x | y; });  break;           // ORR (MOV)
                case 3: bitwise([](u64 x, u64 y, u64) { return x | ~y; }); break;           // ORN
                case 4: bitwise([](u64 x, u64 y, u64) { return x ^ y; });  break;           // EOR
                case 5: bitwise([](u64 x, u64 y, u64 z) { return (x & z) | (y & ~z); }); break; // BSL
                case 6: bitwise([](u64 x, u64 y, u64 z) { return (x & y) | (z & ~y); }); break; // BIT
                default: bitwise([](u64 x, u64 y, u64 z) { return (z & y) | (x & ~y); }); break; // BIF
            }
            write(r);
            return true;
        }

        // --- Coma flotante vectorial (opcode 11xxx) ---
        if (opcode >= 0b11000) {
            const bool fd = size & 1, hi_bit = size >> 1;
            if (fd && !q) return false;
            const unsigned fb = fd ? 8 : 4, fl = total / fb;
            auto fg = [&](const V128& v, unsigned i) { return fd ? F64(v.Get(i, 8)) : double(F32(v.Get(i, 4))); };
            auto fp = [&](V128& v, unsigned i, double x) { v.Set(i, fb, fd ? B64(x) : B32(float(x))); };
            V128 r = Vreg(rd);
            V128 out{};
            for (unsigned i = 0; i < fl; ++i) {
                const double x = fg(a, i), y = fg(b, i);
                double z = 0;
                u64 cmp = 0;
                bool is_cmp = false;
                if (fd) {
                    switch ((u ? 0x40 : 0) | (hi_bit ? 0x20 : 0) | opcode) {
                        case 0x1A: z = x + y; break;                          // FADD
                        case 0x3A: z = x - y; break;                          // FSUB
                        case 0x5B: z = x * y; break;                          // FMUL
                        case 0x5F: z = x / y; break;                          // FDIV
                        case 0x19: z = std::fma(x, y, fg(r, i)); break;       // FMLA
                        case 0x39: z = std::fma(-x, y, fg(r, i)); break;      // FMLS
                        case 0x1E: z = (std::isnan(x) || std::isnan(y)) ? NAN : (x > y ? x : y); break; // FMAX
                        case 0x3E: z = (std::isnan(x) || std::isnan(y)) ? NAN : (x < y ? x : y); break; // FMIN
                        case 0x1C: is_cmp = true; cmp = x == y; break;        // FCMEQ
                        case 0x5C: is_cmp = true; cmp = x >= y; break;        // FCMGE
                        case 0x7C: is_cmp = true; cmp = x > y; break;         // FCMGT
                        case 0x5A: {                                          // FADDP
                            const unsigned half = fl / 2;
                            const V128& src = i < half ? a : b;
                            const unsigned k = (i % half) * 2;
                            z = fg(src, k) + fg(src, k + 1);
                            break;
                        }
                        default: return false;
                    }
                } else {
                    const float xf = float(x), yf = float(y), rf = F32(r.Get(i, 4));
                    float zf = 0;
                    switch ((u ? 0x40 : 0) | (hi_bit ? 0x20 : 0) | opcode) {
                        case 0x1A: zf = xf + yf; break;
                        case 0x3A: zf = xf - yf; break;
                        case 0x5B: zf = xf * yf; break;
                        case 0x5F: zf = xf / yf; break;
                        case 0x19: zf = std::fma(xf, yf, rf); break;
                        case 0x39: zf = std::fma(-xf, yf, rf); break;
                        case 0x1E: zf = (std::isnan(xf) || std::isnan(yf)) ? NAN : (xf > yf ? xf : yf); break;
                        case 0x3E: zf = (std::isnan(xf) || std::isnan(yf)) ? NAN : (xf < yf ? xf : yf); break;
                        case 0x1C: is_cmp = true; cmp = xf == yf; break;
                        case 0x5C: is_cmp = true; cmp = xf >= yf; break;
                        case 0x7C: is_cmp = true; cmp = xf > yf; break;
                        case 0x5A: {
                            const unsigned half = fl / 2;
                            const V128& src = i < half ? a : b;
                            const unsigned k = (i % half) * 2;
                            zf = F32(src.Get(k, 4)) + F32(src.Get(k + 1, 4));
                            break;
                        }
                        default: return false;
                    }
                    z = zf;
                }
                if (is_cmp) out.Set(i, fb, cmp ? Mask(fb) : 0);
                else        fp(out, i, z);
            }
            write(out);
            return true;
        }

        // --- Enteros ---
        V128 r{};
        const V128 d = Vreg(rd);
        for (unsigned i = 0; i < lanes; ++i) {
            const u64 x = a.Get(i, eb), y = b.Get(i, eb);
            const s64 sx = Sx(x, eb), sy = Sx(y, eb);
            u64 z;
            switch (opcode) {
                case 0b10000: z = u ? x - y : x + y; break;                            // SUB / ADD
                case 0b10001: z = (u ? x == y : (x & y) != 0) ? ~0ULL : 0; break;      // CMEQ / CMTST
                case 0b00110: z = (u ? x > y : sx > sy) ? ~0ULL : 0; break;            // CMHI / CMGT
                case 0b00111: z = (u ? x >= y : sx >= sy) ? ~0ULL : 0; break;          // CMHS / CMGE
                case 0b01100: z = u ? (x > y ? x : y) : u64(sx > sy ? sx : sy); break; // UMAX / SMAX
                case 0b01101: z = u ? (x < y ? x : y) : u64(sx < sy ? sx : sy); break; // UMIN / SMIN
                case 0b10011: if (u) return false; z = x * y; break;                   // MUL
                case 0b10010: z = u ? d.Get(i, eb) - x * y : d.Get(i, eb) + x * y; break; // MLS / MLA
                case 0b01000: {                                                         // USHL / SSHL
                    const s8 sh = static_cast<s8>(y & 0xFF);
                    const unsigned bits = eb * 8;
                    if (sh >= 0) z = sh >= int(bits) ? 0 : x << sh;
                    else if (u)  z = -sh >= int(bits) ? 0 : x >> -sh;
                    else         z = u64(sx >> (-sh >= int(bits) ? bits - 1 : unsigned(-sh)));
                    break;
                }
                case 0b10100: case 0b10101: case 0b10111: {                            // pairwise
                    const unsigned half = lanes / 2;
                    const V128& src = i < half ? a : b;
                    const unsigned k = (i % half) * 2;
                    const u64 p = src.Get(k, eb), s2 = src.Get(k + 1, eb);
                    const s64 sp = Sx(p, eb), ss = Sx(s2, eb);
                    if (opcode == 0b10111) { if (u) return false; z = p + s2; }        // ADDP
                    else if (opcode == 0b10100) z = u ? (p > s2 ? p : s2) : u64(sp > ss ? sp : ss); // UMAXP/SMAXP
                    else                        z = u ? (p < s2 ? p : s2) : u64(sp < ss ? sp : ss); // UMINP/SMINP
                    break;
                }
                default: return false;
            }
            r.Set(i, eb, z & Mask(eb));
        }
        write(r);
        return true;
    }

    return false;
}

} // namespace NeXo2::Core
