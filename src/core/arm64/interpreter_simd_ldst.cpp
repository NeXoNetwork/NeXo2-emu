// Lecturas y escrituras de registros SIMD / coma flotante (bit 26 = 1).
// Instrucciones: LDR/STR (b, h, s, d, q) en todas sus formas, LDUR/STUR, LDR (literal),
// LDP/STP (s, d, q), LD1/ST1 (1 a 4 registros y un solo carril).
// memcpy y memset de la libreria de C las usan constantemente.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"

namespace NeXo2::Core {

using namespace NeXo2::Common;

namespace {
// Copia 'bytes' bytes de memoria a un registro V (el resto queda a cero)
V128 LoadV(Memory& m, u64 addr, unsigned bytes) {
    V128 v{};
    m.ReadBytes(addr, &v, bytes);
    return v;
}
void StoreV(Memory& m, u64 addr, const V128& v, unsigned bytes) {
    m.WriteBytes(addr, &v, bytes);
}
} // namespace

bool Interpreter::ExecSimdLoadStore(u32 instr) {
    const u32 rt = Bits(instr, 0, 5);
    const u32 rn = Bits(instr, 5, 5);

    // ------------------------------------------------------------------
    // LD1 / ST1 (estructuras multiples): "ld1 {v0.16b-v3.16b}, [x1], #64"
    //   0 Q 0011000 L 000000 opcode size Rn Rt        (sin post-indice)
    //   0 Q 0011001 L 0 Rm   opcode size Rn Rt        (post-indice)
    // ------------------------------------------------------------------
    if (Bit(instr, 31) == 0 && (Bits(instr, 23, 7) == 0b0011000 || Bits(instr, 23, 7) == 0b0011001)) {
        const bool post  = Bit(instr, 23);
        const bool load  = Bit(instr, 22);
        const bool q     = Bit(instr, 30);
        const u32 opcode = Bits(instr, 12, 4);
        unsigned regs;
        switch (opcode) {
            case 0b0111: regs = 1; break;
            case 0b1010: regs = 2; break;
            case 0b0110: regs = 3; break;
            case 0b0010: regs = 4; break;
            default: return false; // LD2/LD3/LD4 (intercalados): pendiente
        }
        const unsigned bytes = q ? 16 : 8;
        u64 addr = XorSP(rn);
        for (unsigned i = 0; i < regs; ++i) {
            const unsigned t = (rt + i) % 32;
            if (load) Vreg(t) = LoadV(m_memory, addr + i * bytes, bytes);
            else      StoreV(m_memory, addr + i * bytes, Vreg(t), bytes);
        }
        if (post) {
            const u32 rm = Bits(instr, 16, 5);
            SetXorSP(rn, addr + (rm == 31 ? u64(regs * bytes) : X(rm)));
        }
        return true;
    }

    // ------------------------------------------------------------------
    // LD1 / ST1 de un solo carril: "ld1 {v0.b}[6], [x1]"
    //   0 Q 0011010 L R 00000 opcode S size Rn Rt      (sin post-indice)
    //   0 Q 0011011 L R Rm    opcode S size Rn Rt      (post-indice)
    // ------------------------------------------------------------------
    if (Bit(instr, 31) == 0 && (Bits(instr, 23, 7) == 0b0011010 || Bits(instr, 23, 7) == 0b0011011)) {
        if (Bit(instr, 21)) return false;          // LD2/LD4 de carril
        const bool post  = Bit(instr, 23);
        const bool load  = Bit(instr, 22);
        const u32 opcode = Bits(instr, 13, 3);
        const u32 q = Bit(instr, 30), s = Bit(instr, 12), size = Bits(instr, 10, 2);
        unsigned bytes, index;
        switch (opcode) {
            case 0b000: bytes = 1; index = (q << 3) | (s << 2) | size; break;          // .b
            case 0b010: if (size & 1) return false;
                        bytes = 2; index = (q << 2) | (s << 1) | (size >> 1); break;  // .h
            case 0b100: if (size == 0) { bytes = 4; index = (q << 1) | s; break; }    // .s
                        if (size == 1 && s == 0) { bytes = 8; index = q; break; }      // .d
                        return false;
            default: return false; // LD1R y otros: pendiente
        }
        const u64 addr = XorSP(rn);
        if (load) Vreg(rt).Set(index, bytes, ReadMemory(addr, bytes));
        else      WriteMemory(addr, Vreg(rt).Get(index, bytes), bytes);
        if (post) {
            const u32 rm = Bits(instr, 16, 5);
            SetXorSP(rn, addr + (rm == 31 ? u64(bytes) : X(rm)));
        }
        return true;
    }

    const u32 group = Bits(instr, 28, 2); // bits 29..28

    // ------------------------------------------------------------------
    // LDR (literal): s, d o q relativos al PC
    // ------------------------------------------------------------------
    if (group == 0b01 && Bit(instr, 24) == 0) {
        static const unsigned sizes[4] = {4, 8, 16, 0};
        const unsigned bytes = sizes[Bits(instr, 30, 2)];
        if (bytes == 0) return false;
        const u64 addr = m_state.pc + SignExtend(Bits(instr, 5, 19), 19) * 4;
        Vreg(rt) = LoadV(m_memory, addr, bytes);
        return true;
    }

    // ------------------------------------------------------------------
    // LDP / STP de registros SIMD: s, d o q
    // ------------------------------------------------------------------
    if (group == 0b10) {
        const u32 opc = Bits(instr, 30, 2);
        if (opc == 0b11) return false;
        const unsigned bytes = 4u << opc;           // 4, 8, 16
        const u32  type = Bits(instr, 23, 2);       // 00 no-temporal, 01 post, 10 offset, 11 pre
        const bool load = Bit(instr, 22);
        const u32  rt2  = Bits(instr, 10, 5);
        const s64  offset = SignExtend(Bits(instr, 15, 7), 7) * s64(bytes);
        const u64  base = XorSP(rn);
        const u64  addr = (type == 0b01) ? base : base + offset;
        if (load) {
            Vreg(rt)  = LoadV(m_memory, addr, bytes);
            Vreg(rt2) = LoadV(m_memory, addr + bytes, bytes);
        } else {
            StoreV(m_memory, addr, Vreg(rt), bytes);
            StoreV(m_memory, addr + bytes, Vreg(rt2), bytes);
        }
        if (type == 0b01 || type == 0b11) SetXorSP(rn, base + offset);
        return true;
    }

    if (group != 0b11) return false;

    // ------------------------------------------------------------------
    // LDR / STR de un registro SIMD: b, h, s, d, q
    // El tamano sale de size (bits 31..30) y opc<1> (bit 23): 0=b 1=h 2=s 3=d 4=q
    // ------------------------------------------------------------------
    const u32 size  = Bits(instr, 30, 2);
    const u32 opc   = Bits(instr, 22, 2);
    const unsigned scale = ((opc >> 1) << 2) | size;
    if (scale > 4) return false;
    const unsigned bytes = 1u << scale;
    const bool load = opc & 1;

    u64  addr;
    bool writeback = false;
    u64  new_base = 0;
    if (Bit(instr, 24)) {
        addr = XorSP(rn) + (u64(Bits(instr, 10, 12)) << scale);           // [Xn, #imm]
    } else if (Bit(instr, 21) == 0) {
        const s64 imm  = SignExtend(Bits(instr, 12, 9), 9);
        const u32 mode = Bits(instr, 10, 2);
        const u64 base = XorSP(rn);
        switch (mode) {
            case 0b01: addr = base;       writeback = true; new_base = base + imm; break; // post
            case 0b11: addr = base + imm; writeback = true; new_base = addr;       break; // pre
            case 0b00: addr = base + imm; break;                                          // LDUR/STUR
            default:   return false;
        }
    } else if (Bits(instr, 10, 2) == 0b10) {
        const u32 option = Bits(instr, 13, 3);
        if ((option & 0b010) == 0) return false;
        const unsigned shift = Bit(instr, 12) ? scale : 0;
        addr = XorSP(rn) + ExtendReg(Bits(instr, 16, 5), option, shift, true); // [Xn, Xm]
    } else {
        return false;
    }

    if (load) Vreg(rt) = LoadV(m_memory, addr, bytes);
    else      StoreV(m_memory, addr, Vreg(rt), bytes);
    if (writeback) SetXorSP(rn, new_base);
    return true;
}

} // namespace NeXo2::Core
