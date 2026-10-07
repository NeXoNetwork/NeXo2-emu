// Interprete de shaders Maxwell: ejecuta un hilo (un vertice o un pixel).
// Ver shader.hpp y docs/07-nexo-internals/gpu-shaders.md.
#include "shader.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>

namespace NeXo2::GPU {

namespace {

inline float F(u32 v) { float f; std::memcpy(&f, &v, 4); return f; }
inline u32 U(float f) { u32 v; std::memcpy(&v, &f, 4); return v; }

// "Flush to zero": los numeros desnormalizados (muy pequenos) se vuelven 0
inline float Ftz(float x) { return std::fpclassify(x) == FP_SUBNORMAL ? std::copysign(0.0f, x) : x; }
inline float Sat(float x) { return std::isnan(x) ? 0.0f : std::clamp(x, 0.0f, 1.0f); }
inline float AbsNeg(float x, bool abs, bool neg) { if (abs) x = std::fabs(x); return neg ? -x : x; }

// Comparacion de floats (4 bits): o = ordenada (falsa si hay NaN), u = no ordenada
bool FCompare(u32 cond, float a, float b) {
    const bool un = std::isnan(a) || std::isnan(b);
    switch (cond & 0xF) {
        case 0:  return false;
        case 1:  return !un && a < b;
        case 2:  return !un && a == b;
        case 3:  return !un && a <= b;
        case 4:  return !un && a > b;
        case 5:  return !un && a != b;
        case 6:  return !un && a >= b;
        case 7:  return !un;              // NUM: ninguno es NaN
        case 8:  return un;               // NAN
        case 9:  return un || a < b;
        case 10: return un || a == b;
        case 11: return un || a <= b;
        case 12: return un || a > b;
        case 13: return un || a != b;
        case 14: return un || a >= b;
        default: return true;
    }
}

// Comparacion de enteros (3 bits)
bool ICompare(u32 cond, u32 a, u32 b, bool is_signed) {
    const bool lt = is_signed ? s32(a) < s32(b) : a < b;
    const bool eq = a == b;
    switch (cond & 7) {
        case 0: return false;
        case 1: return lt;
        case 2: return eq;
        case 3: return lt || eq;
        case 4: return !lt && !eq;
        case 5: return !eq;
        case 6: return !lt;
        default: return true;
    }
}

bool BoolOp(u32 op, bool a, bool b) {
    switch (op & 3) {
        case 0: return a && b;
        case 1: return a || b;
        case 2: return a != b;
        default: return a;
    }
}

// Media precision (16 bits) <-> float
float HalfToFloat(u16 h) {
    const u32 sign = u32(h >> 15) << 31;
    const u32 e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0) {
        if (m == 0) return F(sign);
        return std::copysign(std::ldexp(float(m), -24), sign ? -1.0f : 1.0f);
    }
    if (e == 31) return F(sign | 0x7F800000u | (m << 13));
    return F(sign | ((e + 112) << 23) | (m << 13));
}
u16 FloatToHalf(float f) {
    const u32 x = U(f);
    const u32 sign = (x >> 16) & 0x8000;
    const s32 e = s32((x >> 23) & 0xFF) - 127 + 15;
    u32 m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return u16(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return u16(sign | 0x7C00);
    if (e <= 0) {
        if (e < -10) return u16(sign);
        m |= 0x800000;
        const u32 shift = u32(14 - e);
        u32 h = m >> shift;
        const u32 rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) ++h;
        return u16(sign | h);
    }
    u32 h = sign | (u32(e) << 10) | (m >> 13);
    const u32 rem = m & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) ++h;
    return u16(h);
}

// Redondeo a entero segun el modo de Maxwell: 0 = al par mas cercano, 1 = hacia -inf,
// 2 = hacia +inf, 3 = hacia cero
double RoundMode(double v, u32 mode) {
    switch (mode & 3) {
        case 0: return std::nearbyint(v);   // el modo por defecto del PC es "al par"
        case 1: return std::floor(v);
        case 2: return std::ceil(v);
        default: return std::trunc(v);
    }
}

u32 ReverseBits(u32 v) {
    u32 r = 0;
    for (int i = 0; i < 32; ++i) r |= ((v >> i) & 1) << (31 - i);
    return r;
}


} // namespace

float ShaderInterpreter::RegF(u32 i) const { return F(Reg(i)); }

ShaderInterpreter::Result ShaderInterpreter::Run(ShaderProgram& prog, ShaderEnv& env) {
    Result res;
    m_r.fill(0);
    m_p.fill(false);
    m_p[7] = true;
    m_ccZero = m_ccSign = m_ccCarry = m_ccOverflow = false;
    auto& stack = m_stack;      // se reutilizan entre ejecuciones (un pixel = una ejecucion)
    auto& local = m_local;      // memoria local (LDL/STL): crece si hace falta
    stack.clear();
    u32 pc = 8;                 // el offset 0 es la palabra de planificacion del primer grupo
    steps = 0;

    auto R = [&](u32 i) -> u32 { return i < 255 ? m_r[i] : 0; };
    auto W = [&](u32 i, u32 v) { if (i < 255) m_r[i] = v; };
    auto P = [&](u32 i) -> bool { return m_p[i & 7]; };
    auto WP = [&](u32 i, bool v) { if ((i & 7) != 7) m_p[i & 7] = v; };
    auto fail = [&](const char* what, const ShaderInstr& in) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s en 0x%X: %s (0x%016llX)", what, pc, ShOpName(in.op),
                      (unsigned long long)in.raw);
        res.ok = false;
        res.error = buf;
    };
    auto next_pc = [](u32 p) { p += 8; if ((p & 31) == 0) p += 8; return p; };   // salta la planificacion
    auto rel_target = [&](const ShaderInstr& in) {   // destino relativo de 24 bits con signo
        const s32 off = s32(in.Bits(20, 24) << 8) >> 8;
        return u32(s32(pc + 8) + off);
    };
    // Saca de la pila hasta encontrar una entrada del tipo pedido
    auto unwind = [&](ShOp kind, u32& target) {
        while (!stack.empty()) {
            const auto e = stack.back();
            stack.pop_back();
            if (e.kind == kind) { target = e.target; return true; }
        }
        return false;
    };

    while (true) {
        if (++steps > max_steps) {
            res.ok = false;
            res.error = "demasiadas instrucciones (bucle infinito?)";
            return res;
        }
        const ShaderInstr& in = prog.At(pc);
        // Predicado de guarda: si es falso, la instruccion no hace nada
        if (in.op != ShOp::Nop && (P(in.pred) == in.pred_neg)) { pc = next_pc(pc); continue; }

        // Operandos habituales
        auto A = [&]() { return R(in.ra); };
        auto B = [&]() -> u32 {
            switch (in.form) {
                case ShForm::Reg: case ShForm::CbufC: return R(in.rb);
                case ShForm::Cbuf: return env.ReadConst(in.cb_index, in.cb_offset);
                case ShForm::Imm: case ShForm::Imm32: return in.imm;
                default: return 0;
            }
        };
        auto C = [&]() -> u32 {
            if (in.form == ShForm::CbufC) return env.ReadConst(in.cb_index, in.cb_offset);
            return R(in.rc);
        };
        auto set_cc_int = [&](u32 v) { m_ccZero = v == 0; m_ccSign = s32(v) < 0; };

        u32 npc = next_pc(pc);
        switch (in.op) {
            // ================= Control de flujo =================
            case ShOp::Nop:
                break;
            case ShOp::Exit:
                if (in.Bits(0, 5) != 0xF && in.Bits(0, 5) != 0) { fail("condicion de EXIT no soportada", in); return res; }
                if (in.Bits(0, 5) == 0xF) return res;
                break;
            case ShOp::Kil:
                if (in.Bits(0, 5) == 0xF) { res.killed = true; return res; }
                break;
            case ShOp::Bra:
                if (in.Bits(0, 5) != 0xF) { fail("condicion de BRA no soportada", in); return res; }
                if (in.Bit(5)) { fail("BRA a traves de constbuf no soportado", in); return res; }
                npc = rel_target(in);
                break;
            case ShOp::Jmp:
                npc = in.Bits(20, 32);
                break;
            case ShOp::Ssy: case ShOp::Pbk: case ShOp::Pcnt:
                stack.push_back({in.op, rel_target(in)});
                break;
            case ShOp::Cal:
                stack.push_back({ShOp::Cal, npc});
                npc = rel_target(in);
                break;
            case ShOp::Sync: case ShOp::Brk: case ShOp::Cont: case ShOp::Ret: {
                const ShOp kind = in.op == ShOp::Sync ? ShOp::Ssy : in.op == ShOp::Brk ? ShOp::Pbk
                                : in.op == ShOp::Cont ? ShOp::Pcnt : ShOp::Cal;
                u32 target = 0;
                if (!unwind(kind, target)) {
                    if (in.op == ShOp::Ret) return res;   // RET sin CAL: fin del programa
                    fail("pila de control vacia", in);
                    return res;
                }
                npc = target;
                break;
            }

            // ================= Movimientos =================
            case ShOp::Mov:
                W(in.rd, B());
                break;
            case ShOp::Mov32i:
                W(in.rd, in.imm);
                break;
            case ShOp::S2R:
                W(in.rd, env.SystemRegister(in.Bits(20, 8)));
                break;
            case ShOp::Sel: {
                const bool p = P(in.Bits(39, 3)) != in.Bit(42);
                W(in.rd, p ? A() : B());
                break;
            }

            // ================= Conversiones =================
            case ShOp::F2F: {
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const u32 b = B();
                float v = src_size == 1 ? HalfToFloat(u16(b >> (in.Bit(41) ? 16 : 0))) : F(b);
                if (in.Bit(44)) v = Ftz(v);
                v = AbsNeg(v, in.Bit(49), in.Bit(45));
                if (in.Bit(42)) v = float(RoundMode(double(v), in.Bits(39, 2)));   // .ri: a entero
                if (in.Bit(50)) v = Sat(v);
                W(in.rd, dst_size == 1 ? u32(FloatToHalf(v)) : U(v));
                break;
            }
            case ShOp::F2I: {
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const bool sgn = in.Bit(12);
                const u32 b = B();
                float v = src_size == 1 ? HalfToFloat(u16(b)) : F(b);
                if (in.Bit(44)) v = Ftz(v);
                v = AbsNeg(v, in.Bit(49), in.Bit(45));
                double r = RoundMode(double(v), in.Bits(39, 2));
                if (dst_size == 3) {   // 64 bits
                    u64 out;
                    if (std::isnan(v)) out = 0;
                    else if (sgn) out = u64(s64(std::clamp(r, -9.2233720368547758e18, 9.2233720368547748e18)));
                    else out = u64(std::clamp(r, 0.0, 1.8446744073709550e19));
                    W(in.rd, u32(out)); W(in.rd + 1, u32(out >> 32));
                } else {
                    const u32 bits = 8u << dst_size;
                    const double lo = sgn ? -std::ldexp(1.0, int(bits) - 1) : 0.0;
                    const double hi = sgn ? std::ldexp(1.0, int(bits) - 1) - 1 : std::ldexp(1.0, int(bits)) - 1;
                    u32 out = std::isnan(v) ? 0 : (sgn ? u32(s32(s64(std::clamp(r, lo, hi)))) : u32(u64(std::clamp(r, lo, hi))));
                    W(in.rd, out);
                }
                set_cc_int(R(in.rd));
                break;
            }
            case ShOp::I2F: {
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const bool sgn = in.Bit(13);
                u32 b = B() >> (in.Bits(41, 2) * 8);
                double v;
                if (src_size == 3) {
                    const u64 w = (u64(R(in.rb + 1)) << 32) | B();
                    v = sgn ? double(s64(w)) : double(w);
                } else {
                    const u32 bits = 8u << src_size;
                    if (bits < 32) b &= (1u << bits) - 1;
                    if (sgn && bits < 32 && (b >> (bits - 1)) & 1) b |= ~((1u << bits) - 1);
                    v = sgn ? double(s32(b)) : double(b);
                }
                if (in.Bit(49)) v = std::fabs(v);
                if (in.Bit(45)) v = -v;
                const float f = float(v);
                W(in.rd, dst_size == 1 ? u32(FloatToHalf(f)) : U(f));
                break;
            }
            case ShOp::I2I: {
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const bool ssgn = in.Bit(13), dsgn = in.Bit(12);
                u32 b = B() >> (in.Bits(41, 2) * 8);
                const u32 sbits = 8u << std::min(src_size, 2u);
                if (sbits < 32) b &= (1u << sbits) - 1;
                s64 v = ssgn && sbits < 32 && ((b >> (sbits - 1)) & 1) ? s64(b) - (s64(1) << sbits) : (ssgn ? s64(s32(b)) : s64(b));
                if (in.Bit(49)) v = v < 0 ? -v : v;
                if (in.Bit(45)) v = -v;
                const u32 dbits = 8u << std::min(dst_size, 2u);
                if (in.Bit(50)) {   // saturar al tipo destino
                    const s64 lo = dsgn ? -(s64(1) << (dbits - 1)) : 0;
                    const s64 hi = dsgn ? (s64(1) << (dbits - 1)) - 1 : (s64(1) << dbits) - 1;
                    v = std::clamp(v, lo, hi);
                }
                u32 out = u32(v);
                if (dbits < 32) {
                    out &= (1u << dbits) - 1;
                    if (dsgn && ((out >> (dbits - 1)) & 1)) out |= ~((1u << dbits) - 1);
                }
                W(in.rd, out);
                set_cc_int(out);
                break;
            }

            // ================= Coma flotante =================
            case ShOp::Fadd: case ShOp::Fadd32i: {
                const bool l = in.op == ShOp::Fadd32i;
                const bool ftz = in.Bit(l ? 55 : 44);
                float a = AbsNeg(F(A()), in.Bit(l ? 54 : 46), in.Bit(l ? 56 : 48));
                float b = AbsNeg(F(B()), in.Bit(l ? 57 : 49), in.Bit(l ? 53 : 45));
                if (ftz) { a = Ftz(a); b = Ftz(b); }
                float r = a + b;
                if (ftz) r = Ftz(r);
                if (!l && in.Bit(50)) r = Sat(r);
                W(in.rd, U(r));
                break;
            }
            case ShOp::Fmul: case ShOp::Fmul32i: {
                const bool l = in.op == ShOp::Fmul32i;
                const u32 fmz = in.Bits(l ? 53 : 44, 2);   // bit 0 = ftz, bit 1 = "fmz" (0 * x = 0)
                float a = F(A()), b = F(B());
                if (!l && in.Bit(48)) a = -a;
                if (fmz) { a = Ftz(a); b = Ftz(b); }
                float r = (fmz & 2) && (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
                if (!l) {   // multiplicar/dividir el resultado por 2, 4 u 8
                    const u32 pdiv = in.Bits(41, 3);
                    if (pdiv >= 1 && pdiv <= 3) r = std::ldexp(r, -int(pdiv));
                    else if (pdiv >= 4 && pdiv <= 6) r = std::ldexp(r, int(7 - pdiv));
                }
                if (fmz) r = Ftz(r);
                if (in.Bit(l ? 55 : 50)) r = Sat(r);
                W(in.rd, U(r));
                break;
            }
            case ShOp::Ffma: case ShOp::Ffma32i: {
                const bool l = in.op == ShOp::Ffma32i;
                const u32 fmz = in.Bits(53, 2);
                float a = F(A()), b = F(B());
                float c = F(l ? R(in.rd) : C());   // FFMA32I: C es el propio destino
                if (in.Bit(l ? 56 : 48)) a = -a;
                if (in.Bit(l ? 57 : 49)) c = -c;
                if (fmz) { a = Ftz(a); b = Ftz(b); c = Ftz(c); }
                float r;
                if ((fmz & 2) && (a == 0.0f || b == 0.0f)) r = c;   // 0 * x = 0
                else r = std::fma(a, b, c);
                if (fmz) r = Ftz(r);
                if (in.Bit(l ? 55 : 50)) r = Sat(r);
                W(in.rd, U(r));
                break;
            }
            case ShOp::Mufu: {
                float a = AbsNeg(F(A()), in.Bit(46), in.Bit(48));
                float r;
                switch (in.Bits(20, 4)) {
                    case 0: r = std::cos(a); break;
                    case 1: r = std::sin(a); break;
                    case 2: r = std::exp2(a); break;
                    case 3: r = std::log2(a); break;
                    case 4: r = 1.0f / a; break;
                    case 5: r = 1.0f / std::sqrt(a); break;
                    case 8: r = std::sqrt(a); break;
                    case 6: case 7:   // RCP64H / RSQ64H: parte alta de un double (shaders con double)
                        fail("MUFU de 64 bits no soportado", in);
                        return res;
                    default: fail("MUFU desconocido", in); return res;
                }
                if (in.Bit(50)) r = Sat(r);
                W(in.rd, U(r));
                break;
            }
            case ShOp::Rro:
                // Prepara el argumento de MUFU (seno/coseno, exp2). Como nuestro MUFU usa
                // las funciones normales, aqui solo se aplican abs y neg.
                W(in.rd, U(AbsNeg(F(B()), in.Bit(49), in.Bit(45))));
                break;
            case ShOp::Fmnmx: {
                float a = AbsNeg(F(A()), in.Bit(46), in.Bit(48));
                float b = AbsNeg(F(B()), in.Bit(49), in.Bit(45));
                if (in.Bit(44)) { a = Ftz(a); b = Ftz(b); }
                const bool min = P(in.Bits(39, 3)) != in.Bit(42);   // cierto = minimo
                W(in.rd, U(min ? std::fmin(a, b) : std::fmax(a, b)));
                break;
            }
            case ShOp::Fset: {
                float a = AbsNeg(F(A()), in.Bit(54), in.Bit(43));
                float b = AbsNeg(F(B()), in.Bit(44), in.Bit(53));
                if (in.Bit(55)) { a = Ftz(a); b = Ftz(b); }
                const bool c = P(in.Bits(39, 3)) != in.Bit(42);
                const bool r = BoolOp(in.Bits(45, 2), FCompare(in.Bits(48, 4), a, b), c);
                W(in.rd, r ? (in.Bit(52) ? U(1.0f) : 0xFFFFFFFFu) : 0);
                break;
            }
            case ShOp::Fsetp: {
                float a = AbsNeg(F(A()), in.Bit(7), in.Bit(43));
                float b = AbsNeg(F(B()), in.Bit(44), in.Bit(6));
                if (in.Bit(47)) { a = Ftz(a); b = Ftz(b); }
                const bool c = P(in.Bits(39, 3)) != in.Bit(42);
                const bool cmp = FCompare(in.Bits(48, 4), a, b);
                const u32 bop = in.Bits(45, 2);
                WP(in.Bits(3, 3), BoolOp(bop, cmp, c));
                WP(in.Bits(0, 3), BoolOp(bop, !cmp, c));
                break;
            }
            case ShOp::Fcmp: {
                float c = F(C());
                if (in.Bit(47)) c = Ftz(c);
                W(in.rd, FCompare(in.Bits(48, 4), c, 0.0f) ? A() : B());
                break;
            }

            // ================= Enteros =================
            case ShOp::Lop: case ShOp::Lop32i: {
                const bool l = in.op == ShOp::Lop32i;
                u32 a = A(), b = B();
                if (in.Bit(l ? 55 : 39)) a = ~a;
                if (in.Bit(l ? 56 : 40)) b = ~b;
                u32 r;
                switch (in.Bits(l ? 53 : 41, 2)) {
                    case 0: r = a & b; break;
                    case 1: r = a | b; break;
                    case 2: r = a ^ b; break;
                    default: r = b; break;   // PASS_B
                }
                W(in.rd, r);
                if (!l) WP(in.Bits(48, 3), r != 0);
                set_cc_int(r);
                break;
            }
            case ShOp::Iadd: case ShOp::Iadd32i: {
                const bool l = in.op == ShOp::Iadd32i;
                u32 a = A(), b = B();
                // Negar A y/o B. Con las dos marcas a la vez es el modo .PO ("plus one"):
                // a + b + 1 (el sumador solo tiene un acarreo de entrada, no puede negar los dos)
                const bool na = in.Bit(l ? 56 : 49), nb = l ? in.Bit(55) : in.Bit(48);
                u32 extra = 0;
                if (na && nb) extra = 1;
                else if (na) a = ~a + 1;
                else if (nb) b = ~b + 1;
                const bool x = in.Bit(l ? 53 : 43);       // .X: sumar el acarreo anterior
                const u64 sum = u64(a) + u64(b) + extra + (x && m_ccCarry ? 1 : 0);
                u32 r = u32(sum);
                if (in.Bit(l ? 54 : 50)) {                // .SAT: saturar con signo
                    const s64 s = s64(s32(a)) + s64(s32(b)) + (x && m_ccCarry ? 1 : 0);
                    r = u32(s32(std::clamp<s64>(s, INT32_MIN, INT32_MAX)));
                }
                if (in.Bit(l ? 52 : 47)) {                // .CC: guardar las banderas
                    m_ccCarry = (sum >> 32) & 1;
                    m_ccOverflow = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
                    set_cc_int(r);
                }
                W(in.rd, r);
                break;
            }
            case ShOp::Imul: case ShOp::Imul32i: {
                const bool l = in.op == ShOp::Imul32i;
                const bool sa = in.Bit(l ? 54 : 40), sb = in.Bit(l ? 55 : 41), hi = in.Bit(l ? 53 : 39);
                const u32 a = A(), b = B();
                const s64 va = sa ? s64(s32(a)) : s64(a), vb = sb ? s64(s32(b)) : s64(b);
                const u64 p = u64(va) * u64(vb);   // en u64: el resultado bajo es el mismo y no desborda
                W(in.rd, hi ? u32(p >> 32) : u32(p));
                break;
            }
            case ShOp::Imad: {
                const bool hi = in.Bit(54), sgn = in.Bit(53) || in.Bit(48);
                const u32 a = A(), b = B(), c = C();
                s64 prod = sgn ? s64(s32(a)) * s64(s32(b)) : s64(u64(a) * u64(b));
                if (in.Bit(52)) prod = -prod;
                u32 r = hi ? u32(u64(prod) >> 32) : u32(prod);
                r += in.Bit(51) ? (~c + 1) : c;
                W(in.rd, r);
                break;
            }
            case ShOp::Iscadd: {
                u32 a = A(), b = B();
                u32 extra = 0;
                if (in.Bit(49) && in.Bit(48)) extra = 1;   // .PO, como en IADD
                else if (in.Bit(49)) a = ~a + 1;
                else if (in.Bit(48)) b = ~b + 1;
                const u32 r = (a << in.Bits(39, 5)) + b + extra;
                W(in.rd, r);
                set_cc_int(r);
                break;
            }
            case ShOp::Xmad: {
                // Multiplicacion de 16x16 bits + suma. Tres XMAD hacen una multiplicacion de 32 bits.
                bool psl = false, mrg = false, h1a = in.Bit(53), h1b = false;
                u32 cmode;
                switch (in.form) {
                    case ShForm::Reg:   psl = in.Bit(36); mrg = in.Bit(37); h1b = in.Bit(35); cmode = in.Bits(50, 3); break;
                    case ShForm::Imm:   psl = in.Bit(36); mrg = in.Bit(37); cmode = in.Bits(50, 3); break;
                    case ShForm::Cbuf:  psl = in.Bit(55); mrg = in.Bit(56); h1b = in.Bit(52); cmode = in.Bits(50, 2); break;
                    default:            h1b = in.Bit(52); cmode = in.Bits(50, 2); break;   // CbufC
                }
                const bool sa = in.Bit(48), sb = in.Bit(49);
                const u32 a = A(), b = B(), c = C();
                auto half = [](u32 v, bool high, bool sgn) -> u32 {
                    const u32 h = high ? v >> 16 : v & 0xFFFF;
                    return sgn ? u32(s32(s16(u16(h)))) : h;
                };
                const u32 ha = half(a, h1a, sa), hb = half(b, h1b, sb);
                u32 prod = ha * hb;
                if (psl) prod <<= 16;
                u32 cc = c;
                switch (cmode) {
                    case 1: cc = c & 0xFFFF; break;                 // CLO
                    case 2: cc = c >> 16; break;                    // CHI
                    case 3:                                         // CSFU
                        if (ha != 0 && hb != 0) {
                            if (sa && (ha >> 31)) cc -= 0x10000;
                            if (sb && (hb >> 31)) cc -= 0x10000;
                        }
                        break;
                    case 4: cc = c + (b << 16); break;              // CBCC
                    default: break;
                }
                u32 r = prod + cc;
                if (mrg) r = (r & 0xFFFF) | (b << 16);
                W(in.rd, r);
                set_cc_int(r);
                break;
            }
            case ShOp::Imnmx: {
                const bool sgn = in.Bit(48);
                const u32 a = A(), b = B();
                const bool min = P(in.Bits(39, 3)) != in.Bit(42);
                const bool a_lt = sgn ? s32(a) < s32(b) : a < b;
                W(in.rd, min ? (a_lt ? a : b) : (a_lt ? b : a));
                break;
            }
            case ShOp::Iset: {
                const bool c = P(in.Bits(39, 3)) != in.Bit(42);
                const bool r = BoolOp(in.Bits(45, 2), ICompare(in.Bits(49, 3), A(), B(), in.Bit(48)), c);
                W(in.rd, r ? (in.Bit(44) ? U(1.0f) : 0xFFFFFFFFu) : 0);
                break;
            }
            case ShOp::Isetp: {
                const bool c = P(in.Bits(39, 3)) != in.Bit(42);
                const bool cmp = ICompare(in.Bits(49, 3), A(), B(), in.Bit(48));
                const u32 bop = in.Bits(45, 2);
                WP(in.Bits(3, 3), BoolOp(bop, cmp, c));
                WP(in.Bits(0, 3), BoolOp(bop, !cmp, c));
                break;
            }
            case ShOp::Icmp:
                W(in.rd, ICompare(in.Bits(49, 3), C(), 0, in.Bit(48)) ? A() : B());
                break;
            case ShOp::Shl: {
                const u32 a = A(), s = B();
                const u32 r = in.Bit(39) ? a << (s & 31) : (s >= 32 ? 0 : a << s);
                W(in.rd, r);
                set_cc_int(r);
                break;
            }
            case ShOp::Shr: {
                const u32 a = A();
                u32 s = B();
                if (in.Bit(39)) s &= 31;
                u32 r;
                if (in.Bit(48)) r = u32(s32(a) >> std::min(s, 31u));
                else r = s >= 32 ? 0 : a >> s;
                W(in.rd, r);
                set_cc_int(r);
                break;
            }
            case ShOp::Popc: {
                u32 b = B();
                if (in.Bit(40)) b = ~b;
                W(in.rd, u32(std::popcount(b)));
                break;
            }
            case ShOp::Bfi: {
                const u32 a = A(), b = B(), c = C();
                const u32 pos = b & 0xFF, len = (b >> 8) & 0xFF;
                u32 r = c;
                if (len && pos < 32) {
                    const u32 n = std::min(len, 32 - pos);
                    const u32 mask = (n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1)) << pos;
                    r = (c & ~mask) | ((a << pos) & mask);
                }
                W(in.rd, r);
                set_cc_int(r);
                break;
            }
            case ShOp::Bfe: {
                u32 a = A();
                const u32 b = B();
                if (in.Bit(40)) a = ReverseBits(a);   // .BREV: dar la vuelta a los bits antes
                const u32 pos = b & 0xFF, len = (b >> 8) & 0xFF;
                const bool sgn = in.Bit(48);
                u32 r = 0;
                if (len) {
                    if (pos >= 32) r = sgn && (a >> 31) ? 0xFFFFFFFFu : 0;
                    else {
                        const u32 n = std::min(len, 32 - pos);
                        r = n >= 32 ? a : (a >> pos) & ((1u << n) - 1);
                        if (sgn && n < 32 && ((r >> (n - 1)) & 1)) r |= ~((1u << n) - 1);
                        if (sgn && len > 32 - pos && (a >> 31)) r |= ~((1u << n) - 1);
                    }
                }
                W(in.rd, r);
                set_cc_int(r);
                break;
            }
            case ShOp::Flo: {
                u32 b = B();
                if (in.Bit(40)) b = ~b;
                if (in.Bit(48) && s32(b) < 0) b = ~b;   // con signo: buscar el primer bit distinto del signo
                u32 r = b ? u32(31 - std::countl_zero(b)) : 0xFFFFFFFFu;
                if (in.Bit(41) && r != 0xFFFFFFFFu) r = 31 - r;   // .SH: distancia desde arriba
                W(in.rd, r);
                break;
            }

            // ================= Atributos y memoria =================
            case ShOp::Ald: {
                const u32 count = in.Bits(47, 2) + 1;
                const u32 addr = in.Bits(20, 10) + R(in.ra);
                for (u32 i = 0; i < count; ++i) W(in.rd + i, env.ReadAttribute(addr + 4 * i));
                break;
            }
            case ShOp::Ast: {
                const u32 count = in.Bits(47, 2) + 1;
                const u32 addr = in.Bits(20, 10) + R(in.ra);
                for (u32 i = 0; i < count; ++i) env.WriteAttribute(addr + 4 * i, R(in.rd + i));
                break;
            }
            case ShOp::Ipa: {
                const u32 addr = in.Bits(28, 10) + (in.Bit(38) ? R(in.ra) : 0);
                const u32 mode = in.Bits(54, 2);
                float v = env.Interpolate(addr, mode);
                if (mode == 1 || (mode == 3 && in.Bits(20, 8) != 0xFF)) v *= F(R(in.Bits(20, 8)));
                if (in.Bit(51)) v = Sat(v);
                W(in.rd, U(v));
                break;
            }
            case ShOp::Ldc: {
                const u32 size = in.Bits(48, 3);
                const u32 idx = in.Bits(36, 5);
                const u32 addr = u32(s32(s16(u16(in.Bits(20, 16))))) + R(in.ra);
                const u32 words = size == 5 ? 2 : size == 6 ? 4 : 1;
                for (u32 i = 0; i < words; ++i) {
                    u32 v = env.ReadConst(idx, (addr & ~3u) + 4 * i);
                    if (size < 4) {   // 8 o 16 bits
                        v >>= (addr & 3) * 8;
                        if (size == 0) v &= 0xFF;
                        else if (size == 1) v = u32(s32(s8(u8(v))));
                        else if (size == 2) v &= 0xFFFF;
                        else v = u32(s32(s16(u16(v))));
                    }
                    W(in.rd + i, v);
                }
                break;
            }
            case ShOp::Ldl: case ShOp::Stl: {
                const u32 size = in.Bits(48, 3);
                const u32 addr = u32(s32(in.Bits(20, 24) << 8) >> 8) + R(in.ra);
                const u32 bytes = size <= 1 ? 1 : size <= 3 ? 2 : size == 4 ? 4 : size == 5 ? 8 : 16;
                if (addr + bytes > 0x10000) { fail("memoria local fuera de rango", in); return res; }
                if (local.size() < addr + bytes) local.resize(std::max<size_t>(addr + bytes, 256), 0);
                if (in.op == ShOp::Stl) {
                    for (u32 i = 0; i < std::max(bytes / 4, 1u); ++i) {
                        const u32 v = R(in.rd + i);
                        std::memcpy(&local[addr + 4 * i], &v, std::min(bytes, 4u));
                    }
                } else {
                    for (u32 i = 0; i < std::max(bytes / 4, 1u); ++i) {
                        u32 v = 0;
                        std::memcpy(&v, &local[addr + 4 * i], std::min(bytes, 4u));
                        if (size == 1) v = u32(s32(s8(u8(v))));
                        if (size == 3) v = u32(s32(s16(u16(v))));
                        W(in.rd + i, v);
                    }
                }
                break;
            }

            // ================= Texturas =================
            case ShOp::Texs: case ShOp::Tlds: case ShOp::Tex: {
                fail("las texturas llegan en la GPU fase 2b", in);
                return res;
            }

            default:
                fail("instruccion desconocida", in);
                return res;
        }
        pc = npc;
    }
}

} // namespace NeXo2::GPU
