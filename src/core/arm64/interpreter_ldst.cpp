// Lecturas y escrituras en memoria (bit 27 = 1, bit 25 = 0).
// Instrucciones: LDR/STR (todas sus formas), LDRB/LDRH/LDRSB/LDRSH/LDRSW, LDUR/STUR,
// LDP/STP/LDPSW, LDR (literal), LDXR/STXR, LDAR/STLR, CAS, LDADD/SWP y demas atomicos.
// Las versiones con registros SIMD (q0, d0...) estan en interpreter_simd_ldst.cpp.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"

namespace NeXo2::Core {

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
    if (group == 0b01 && Bit(instr, 24) == 0) {
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
        const u64 address = XorSP(rn);
        const u64 old = ReadMemory(address, bytes);
        const u64 mask = Ones(bytes * 8);
        const u64 operand = X(rs) & mask;
        const s64 sold = SignExtend(old, bytes * 8), sop = SignExtend(operand, bytes * 8);
        u64 result;
        if (o3) {
            if (aop != 0) return false;   // LDAPR y otros: pendiente
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
        WriteMemory(address, result & mask, bytes);
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

    if (opc == 0b11 || (opc == 0b01 && !load)) return false;

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
    const u32  rn   = Bits(instr, 5, 5);
    const u32  rt   = Bits(instr, 0, 5);
    const unsigned bytes = 1u << size;
    const u64 addr = XorSP(rn);

    if (!o2 && !o1) {
        if (load) {
            // LDXR / LDAXR: lee y "reserva" la direccion
            SetX(rt, ReadMemory(addr, bytes));
            m_exclusiveValid = true;
            m_exclusiveAddr  = addr;
        } else {
            // STXR / STLXR: escribe solo si la reserva sigue viva. Ws = 0 exito, 1 fallo.
            if (m_exclusiveValid && m_exclusiveAddr == addr) {
                WriteMemory(addr, X(rt), bytes);
                SetX(rs, 0, false);
            } else {
                SetX(rs, 1, false);
            }
            m_exclusiveValid = false;
        }
        return true;
    }

    if (o2 && !o1) {
        // LDAR / STLR (y LDLAR/STLLR): acceso normal con orden de memoria.
        // Con un solo nucleo el orden ya es secuencial.
        if (load) SetX(rt, ReadMemory(addr, bytes));
        else      WriteMemory(addr, X(rt), bytes);
        return true;
    }

    if (o2 && o1 && Bits(instr, 10, 5) == 0b11111) {
        // CAS / CASA / CASL / CASAL: si mem == Rs, mem = Rt. Rs recibe el valor viejo.
        const u64 mask = Ones(bytes * 8);
        const u64 old  = ReadMemory(addr, bytes);
        if (old == (X(rs) & mask)) WriteMemory(addr, X(rt) & mask, bytes);
        SetX(rs, old, size == 3);
        return true;
    }

    return false; // LDXP/STXP, CASP: pendiente
}

} // namespace NeXo2::Core
