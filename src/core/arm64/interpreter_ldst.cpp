// Lecturas y escrituras en memoria (bit 27 = 1, bit 25 = 0).
// Instrucciones: LDR/STR (todas sus formas), LDRB/LDRH/LDRSB/LDRSH/LDRSW, LDUR/STUR,
// LDP/STP/LDPSW, LDR (literal), LDXR/STXR, LDAR/STLR, CAS, LDADD/SWP y demas atomicos.
// Las versiones con registros SIMD (q0, d0...) estan en interpreter_simd_ldst.cpp.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"

namespace NeXo2::Core {

// Compare-and-swap atomico de 1, 2, 4 u 8 bytes. Si falla, 'expected' recibe el valor actual.
static bool CasMemory(Memory& mem, u64 addr, unsigned bytes, u64& expected, u64 desired) {
    switch (bytes) {
        case 1: { u8  e = u8(expected);  const bool ok = mem.CompareExchange<u8>(addr, e, u8(desired));   expected = e; return ok; }
        case 2: { u16 e = u16(expected); const bool ok = mem.CompareExchange<u16>(addr, e, u16(desired)); expected = e; return ok; }
        case 4: { u32 e = u32(expected); const bool ok = mem.CompareExchange<u32>(addr, e, u32(desired)); expected = e; return ok; }
        default: return mem.CompareExchange<u64>(addr, expected, desired);
    }
}


using namespace NeXo2::Common;

namespace {
// Escribe en Rt un valor leido de memoria aplicando la extension que pide 'opc'.
//   opc 01 -> sin signo (LDR, LDRB, LDRH)
//   opc 10 -> con signo a 64 bits (LDRSB/LDRSH/LDRSW Xt)
//   opc 11 -> con signo a 32 bits (LDRSB/LDRSH Wt)
u64 ExtendLoaded(u64 raw, unsigned size_bytes, u32 opc, bool& sf_out) {
    switch (opc) {
        case 0b10: sf_out = true;  return u64(SignExtend(raw, size_bytes * 8));
        case 0b11: sf_out = false; return u64(SignExtend(raw, size_bytes * 8)) & 0xFFFFFFFFu;
        default:   sf_out = true;  return raw;
    }
}
} // namespace

bool Interpreter::ExecLoadStore(u32 instr) {
    const bool is_simd = Bit(instr, 26);
    if (is_simd) return ExecSimdLoadStore(instr); // q0/d0/s0..., ld1/st1: interpreter_simd_ldst.cpp

    const u32 group = Bits(instr, 28, 2); // bits 29..28
    const u32 rt = Bits(instr, 0, 5);
    const u32 rn = Bits(instr, 5, 5);

    // ------------------------------------------------------------------
    // Exclusivas, acquire/release y CAS (bits 29..24 = 001000)
    // ------------------------------------------------------------------
    if (Bits(instr, 24, 6) == 0b001000) return ExecLoadStoreExclusive(instr);

    // ------------------------------------------------------------------
    // LDR (literal): lee una constante situada cerca del codigo
    // ------------------------------------------------------------------
    if (group == 0b01 && Bits(instr, 24, 2) == 0) {
        const u32 opc = Bits(instr, 30, 2);
        const u64 addr = m_state.pc + SignExtend(Bits(instr, 5, 19), 19) * 4;
        switch (opc) {
            case 0b00: SetX(rt, ReadMemory(addr, 4)); return true;                       // LDR Wt
            case 0b01: SetX(rt, ReadMemory(addr, 8)); return true;                       // LDR Xt
            case 0b10: SetX(rt, u64(SignExtend(ReadMemory(addr, 4), 32))); return true;  // LDRSW
            default:   return true;                                                      // PRFM (pista de cache)
        }
    }

    // ------------------------------------------------------------------
    // LDP / STP: dos registros a la vez (prologos y epilogos de funciones)
    // ------------------------------------------------------------------
    if (group == 0b10) return ExecLoadStorePair(instr);

    if (group != 0b11) return false;

    // ------------------------------------------------------------------
    // Un registro: LDR/STR y variantes
    // ------------------------------------------------------------------
    const u32 size = Bits(instr, 30, 2);      // 0=byte, 1=16 bits, 2=32 bits, 3=64 bits
    const u32 opc  = Bits(instr, 22, 2);      // 00=store, 01=load, 10/11=load con signo
    const unsigned bytes = 1u << size;

    u64  addr;
    bool writeback = false;
    u64  new_base  = 0;

    if (Bit(instr, 24)) {
        // [Xn, #imm12 * tamano]  (offset sin signo escalado)
        addr = XorSP(rn) + (u64(Bits(instr, 10, 12)) << size);
    } else if (Bit(instr, 21) == 0) {
        // Offset de 9 bits con signo: LDUR/STUR, post-indice [Xn], #imm y pre-indice [Xn, #imm]!
        const s64 imm  = SignExtend(Bits(instr, 12, 9), 9);
        const u32 mode = Bits(instr, 10, 2);
        const u64 base = XorSP(rn);
        switch (mode) {
            case 0b01: addr = base;       writeback = true; new_base = base + imm; break; // post
            case 0b11: addr = base + imm; writeback = true; new_base = addr;       break; // pre
            default:   addr = base + imm; break;                                          // LDUR / LDTR
        }
        if (writeback && opc == 0b10 && size == 3) return false;
        if (mode == 0b10 && opc == 0b10 && size == 3) return false;   // LDTR: no hay PRFM sin privilegios
    } else if (Bits(instr, 10, 2) == 0b10) {
        // [Xn, Xm{, LSL #n}] / [Xn, Wm, SXTW #n]  (offset con registro)
        const u32 option = Bits(instr, 13, 3);
        if ((option & 0b010) == 0) return false;
        const unsigned shift = Bit(instr, 12) ? size : 0;
        addr = XorSP(rn) + ExtendReg(Bits(instr, 16, 5), option, shift, true);
    } else if (Bits(instr, 10, 2) == 0b00) {
        // Atomicos LSE: LDADD, LDCLR, LDEOR, LDSET, LDSMAX/MIN, LDUMAX/MIN, SWP
        // Leen el valor viejo en Rt y guardan en memoria op(viejo, Rs).
        const u32 rs = Bits(instr, 16, 5);
        const u32 o3 = Bit(instr, 15);
        const u32 aop = Bits(instr, 12, 3);
        if (o3 && aop == 0b100) {
            // LDAPR: lectura con "acquire" (ARMv8.3). Rs = 11111, A = 1, R = 0.
            if (rs != 0b11111 || !Bit(instr, 23) || Bit(instr, 22)) return false;
            SetX(rt, ReadMemory(XorSP(rn), bytes));
            return true;
        }
        if (o3 && aop != 0) return false;   // LDAPR ya visto; el resto no existe
        const u64 address = XorSP(rn);
        const u64 mask = Ones(bytes * 8);
        const u64 operand = X(rs) & mask;
        // Leer, calcular y escribir de forma atomica (otro nucleo puede tocar la misma
        // direccion): se repite si la memoria cambio entre medias.
        u64 old = ReadMemory(address, bytes);
        for (;;) {
            const s64 sold = SignExtend(old, bytes * 8), sop = SignExtend(operand, bytes * 8);
            u64 result;
            if (o3) {
                result = operand;             // SWP
            } else {
                switch (aop) {
                    case 0b000: result = old + operand;               break; // LDADD
                    case 0b001: result = old & ~operand;              break; // LDCLR
                    case 0b010: result = old ^ operand;               break; // LDEOR
                    case 0b011: result = old | operand;               break; // LDSET
                    case 0b100: result = (sold > sop) ? old : operand; break; // LDSMAX
                    case 0b101: result = (sold < sop) ? old : operand; break; // LDSMIN
                    case 0b110: result = (old > operand) ? old : operand; break; // LDUMAX
                    default:    result = (old < operand) ? old : operand; break; // LDUMIN
                }
            }
            if (CasMemory(m_memory, address, bytes, old, result & mask)) break;   // si no, 'old' = actual
        }
        if (m_exclusiveValid && m_exclusiveAddr == address) m_exclusiveValid = false;
        SetX(rt, old);
        return true;
    } else {
        return false;
    }

    // Ejecutar el acceso
    if (opc == 0b00) {
        WriteMemory(addr, X(rt), bytes);                              // STR / STRB / STRH
    } else if (opc == 0b10 && size == 3) {
        // PRFM: solo es una pista para la cache, no hace nada
    } else if (opc == 0b11 && size >= 2) {
        return false;
    } else {
        bool sf = true;
        const u64 value = ExtendLoaded(ReadMemory(addr, bytes), bytes, opc, sf);
        SetX(rt, value, sf);                                          // LDR / LDRB / LDRSW...
    }

    if (writeback) SetXorSP(rn, new_base);
    return true;
}

bool Interpreter::ExecLoadStorePair(u32 instr) {
    const u32  opc  = Bits(instr, 30, 2);     // 00 = W, 01 = LDPSW, 10 = X
    const u32  type = Bits(instr, 23, 2);     // 00 no-temporal, 01 post, 10 offset, 11 pre
    const bool load = Bit(instr, 22);
    const u32  rt   = Bits(instr, 0, 5);
    const u32  rt2  = Bits(instr, 10, 5);
    const u32  rn   = Bits(instr, 5, 5);

    if (Bit(instr, 25)) return false;                       // bits 25..23 = 1xx: no existe
    if (opc == 0b11 || (opc == 0b01 && (!load || type == 0b00))) return false;   // LDPSW no tiene version no-temporal

    const unsigned bytes = (opc == 0b10) ? 8 : 4;
    const s64 offset = SignExtend(Bits(instr, 15, 7), 7) * s64(bytes);
    const u64 base = XorSP(rn);
    const u64 addr = (type == 0b01) ? base : base + offset; // post-indice usa la base tal cual

    if (load) {
        u64 a = ReadMemory(addr, bytes);
        u64 b = ReadMemory(addr + bytes, bytes);
        if (opc == 0b01) { // LDPSW
            a = u64(SignExtend(a, 32));
            b = u64(SignExtend(b, 32));
        }
        SetX(rt, a);
        SetX(rt2, b);
    } else {
        WriteMemory(addr, X(rt), bytes);
        WriteMemory(addr + bytes, X(rt2), bytes);
    }

    if (type == 0b01 || type == 0b11) SetXorSP(rn, base + offset); // "!" o post-indice
    return true;
}

bool Interpreter::ExecLoadStoreExclusive(u32 instr) {
    const u32  size = Bits(instr, 30, 2);
    const bool o2   = Bit(instr, 23);
    const bool load = Bit(instr, 22);
    const bool o1   = Bit(instr, 21);
    const u32  rs   = Bits(instr, 16, 5);
    const u32  rt2  = Bits(instr, 10, 5);
    const u32  rn   = Bits(instr, 5, 5);
    const u32  rt   = Bits(instr, 0, 5);
    const unsigned bytes = 1u << size;
    const u64 addr = XorSP(rn);

    if (!o2 && !o1) {
        if (load) {
            // LDXR / LDAXR: lee y "reserva" la direccion (y recuerda el valor)
            const u64 v = ReadMemory(addr, bytes);
            SetX(rt, v);
            m_exclusiveValid = true;
            m_exclusiveAddr  = addr;
            m_exclusiveValue[0] = v;
        } else {
            // STXR / STLXR: escribe solo si la reserva sigue viva y nadie (ni otro nucleo)
            // ha cambiado la memoria desde el LDXR. Ws = 0 exito, 1 fallo.
            u64 expected = m_exclusiveValue[0];
            const bool ok = m_exclusiveValid && m_exclusiveAddr == addr &&
                            CasMemory(m_memory, addr, bytes, expected, X(rt) & Ones(bytes * 8));
            SetX(rs, ok ? 0 : 1, false);
            m_exclusiveValid = false;
        }
        return true;
    }

    if (!o2 && o1 && (size & 2)) {
        // LDXP / LDAXP / STXP / STLXP: dos registros (W o X) a la vez
        const bool sf = size & 1;
        const unsigned eb = sf ? 8 : 4;
        if (load) {
            const u64 lo = ReadMemory(addr, eb), hi = ReadMemory(addr + eb, eb);
            SetX(rt, lo, sf);
            SetX(rt2, hi, sf);
            m_exclusiveValid = true;
            m_exclusiveAddr  = addr;
            m_exclusiveValue[0] = lo;
            m_exclusiveValue[1] = hi;
        } else {
            bool ok = m_exclusiveValid && m_exclusiveAddr == addr;
            if (ok) {
                const u64 lo = X(rt, sf), hi = X(rt2, sf);   // leer antes: Rs podria ser uno de ellos
                if (sf) {
                    u64 el = m_exclusiveValue[0], eh = m_exclusiveValue[1];
                    ok = m_memory.CompareExchange128(addr, el, eh, lo, hi);
                } else {   // dos W = 8 bytes: un solo CAS de 64 bits
                    u64 e = (m_exclusiveValue[0] & 0xFFFFFFFFu) | (m_exclusiveValue[1] << 32);
                    ok = m_memory.CompareExchange<u64>(addr, e, (lo & 0xFFFFFFFFu) | (hi << 32));
                }
            }
            SetX(rs, ok ? 0 : 1, false);
            m_exclusiveValid = false;
        }
        return true;
    }

    if (!o2 && o1) {
        // CASP / CASPA / CASPL / CASPAL: compara y cambia una pareja de registros (pares)
        if (rt2 != 0b11111 || (rs & 1) || (rt & 1)) return false;
        const bool sf = Bit(instr, 30);
        const unsigned eb = sf ? 8 : 4;
        const u64 mask = Ones(eb * 8);
        u64 old_lo = X(rs, sf) & mask, old_hi = X(rs + 1, sf) & mask;
        const u64 new_lo = X(rt, sf) & mask, new_hi = X(rt + 1, sf) & mask;
        if (sf) {
            m_memory.CompareExchange128(addr, old_lo, old_hi, new_lo, new_hi);   // si falla: valores actuales
        } else {
            u64 e = old_lo | (old_hi << 32);
            m_memory.CompareExchange<u64>(addr, e, new_lo | (new_hi << 32));
            old_lo = e & 0xFFFFFFFFu;
            old_hi = e >> 32;
        }
        SetX(rs, old_lo, sf);
        SetX(rs + 1, old_hi, sf);
        return true;
    }

    if (o2 && !o1) {
        // LDAR / STLR (y LDLAR/STLLR): acceso normal con orden de memoria.
        // Rs y Rt2 tienen que ser 11111 (si no, la instruccion no es valida).
        if (rs != 0b11111 || rt2 != 0b11111) return false;
        if (load) SetX(rt, ReadMemory(addr, bytes));
        else      WriteMemory(addr, X(rt), bytes);
        return true;
    }

    if (rt2 == 0b11111) {
        // CAS / CASA / CASL / CASAL: si mem == Rs, mem = Rt. Rs recibe el valor viejo.
        const u64 mask = Ones(bytes * 8);
        u64 old = X(rs) & mask;
        CasMemory(m_memory, addr, bytes, old, X(rt) & mask);   // si falla, 'old' = valor actual
        SetX(rs, old, size == 3);
        return true;
    }
    return false;
}

} // namespace NeXo2::Core
