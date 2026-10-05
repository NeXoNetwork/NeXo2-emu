// Lecturas y escrituras de registros SIMD / coma flotante (bit 26 = 1).
// Instrucciones: LDR/STR (b, h, s, d, q) en todas sus formas, LDUR/STUR, LDR (literal),
// LDP/STP (s, d, q), LD1-LD4/ST1-ST4 (todas las formas) y LD1R-LD4R.
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
    // LD1-LD4 / ST1-ST4 (estructuras multiples): "ld1 {v0.16b-v3.16b}, [x1], #64",
    // "ld3 {v0.8b-v2.8b}, [x0]" (LD2/3/4 reparten los elementos entre los registros:
    // ideal para separar canales R,G,B de una imagen).
    //   0 Q 0011000 L 000000 opcode size Rn Rt        (sin post-indice)
    //   0 Q 0011001 L 0 Rm   opcode size Rn Rt        (post-indice)
    // ------------------------------------------------------------------
    if (Bit(instr, 31) == 0 && (Bits(instr, 23, 7) == 0b0011000 || Bits(instr, 23, 7) == 0b0011001)) {
        const bool post  = Bit(instr, 23);
        const bool load  = Bit(instr, 22);
        const bool q     = Bit(instr, 30);
        if (post ? Bit(instr, 21) : Bits(instr, 16, 6) != 0) return false;
        const u32 opcode = Bits(instr, 12, 4), size = Bits(instr, 10, 2);
        unsigned rpt, selem;   // repeticiones y registros por estructura
        switch (opcode) {
            case 0b0000: rpt = 1; selem = 4; break;   // LD4/ST4
            case 0b0010: rpt = 4; selem = 1; break;   // LD1/ST1 de 4 registros
            case 0b0100: rpt = 1; selem = 3; break;   // LD3/ST3
            case 0b0110: rpt = 3; selem = 1; break;   // LD1/ST1 de 3 registros
            case 0b0111: rpt = 1; selem = 1; break;   // LD1/ST1
            case 0b1000: rpt = 1; selem = 2; break;   // LD2/ST2
            case 0b1010: rpt = 2; selem = 1; break;   // LD1/ST1 de 2 registros
            default: return false;
        }
        if (size == 3 && !q && selem != 1) return false;
        const unsigned ebytes = 1u << size, elements = (q ? 16 : 8) / ebytes;
        const u64 addr = XorSP(rn);
        u64 offs = 0;
        if (load && selem == 1) {
            // Caso comun (memcpy): registros enteros seguidos
            for (unsigned r = 0; r < rpt; ++r, offs += elements * ebytes)
                Vreg((rt + r) % 32) = LoadV(m_memory, addr + offs, elements * ebytes);
        } else if (!load && selem == 1) {
            for (unsigned r = 0; r < rpt; ++r, offs += elements * ebytes)
                StoreV(m_memory, addr + offs, Vreg((rt + r) % 32), elements * ebytes);
        } else {
            // Intercalado: elemento e de cada registro, uno tras otro
            V128 regs[4];
            for (unsigned s = 0; s < selem; ++s) regs[s] = load ? V128{} : Vreg((rt + s) % 32);
            for (unsigned e = 0; e < elements; ++e)
                for (unsigned s = 0; s < selem; ++s, offs += ebytes) {
                    if (load) regs[s].Set(e, ebytes, ReadMemory(addr + offs, ebytes));
                    else      WriteMemory(addr + offs, regs[s].Get(e, ebytes), ebytes);
                }
            if (load)
                for (unsigned s = 0; s < selem; ++s) Vreg((rt + s) % 32) = regs[s];
        }
        if (post) {
            const u32 rm = Bits(instr, 16, 5);
            SetXorSP(rn, addr + (rm == 31 ? offs : X(rm)));
        }
        return true;
    }

    // ------------------------------------------------------------------
    // LD1-LD4 / ST1-ST4 de un carril y LD1R-LD4R (cargar y repetir en todos los carriles):
    // "ld1 {v0.b}[6], [x1]", "ld4r {v0.4s-v3.4s}, [x2]"
    //   0 Q 0011010 L R 00000 opcode S size Rn Rt      (sin post-indice)
    //   0 Q 0011011 L R Rm    opcode S size Rn Rt      (post-indice)
    // ------------------------------------------------------------------
    if (Bit(instr, 31) == 0 && (Bits(instr, 23, 7) == 0b0011010 || Bits(instr, 23, 7) == 0b0011011)) {
        const bool post  = Bit(instr, 23);
        const bool load  = Bit(instr, 22);
        if (!post && Bits(instr, 16, 5) != 0) return false;
        const u32 opcode = Bits(instr, 13, 3);
        const u32 q = Bit(instr, 30), s = Bit(instr, 12), size = Bits(instr, 10, 2);
        const unsigned selem = (((opcode & 1) << 1) | Bit(instr, 21)) + 1;
        unsigned scale = opcode >> 1, index = 0;
        bool replicate = false;
        switch (scale) {
            case 3:   // LDnR
                if (!load || s) return false;
                scale = size;
                replicate = true;
                break;
            case 0: index = (q << 3) | (s << 2) | size; break;                       // .b
            case 1: if (size & 1) return false;
                    index = (q << 2) | (s << 1) | (size >> 1); break;                 // .h
            case 2: if (size & 2) return false;
                    if ((size & 1) == 0) { index = (q << 1) | s; break; }           // .s
                    if (s) return false;
                    index = q; scale = 3; break;                                       // .d
        }
        const unsigned ebytes = 1u << scale;
        const u64 addr = XorSP(rn);
        u64 offs = 0;
        for (unsigned e = 0; e < selem; ++e, offs += ebytes) {
            const unsigned t = (rt + e) % 32;
            if (replicate) {
                const u64 value = ReadMemory(addr + offs, ebytes);
                V128 v{};
                for (unsigned i = 0; i < (q ? 16u : 8u) / ebytes; ++i) v.Set(i, ebytes, value);
                Vreg(t) = v;
            } else if (load) {
                Vreg(t).Set(index, ebytes, ReadMemory(addr + offs, ebytes));
            } else {
                WriteMemory(addr + offs, Vreg(t).Get(index, ebytes), ebytes);
            }
        }
        if (post) {
            const u32 rm = Bits(instr, 16, 5);
            SetXorSP(rn, addr + (rm == 31 ? offs : X(rm)));
        }
        return true;
    }

    const u32 group = Bits(instr, 28, 2); // bits 29..28

    // ------------------------------------------------------------------
    // LDR (literal): s, d o q relativos al PC
    // ------------------------------------------------------------------
    if (group == 0b01 && Bits(instr, 24, 2) == 0) {
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
        if (opc == 0b11 || Bit(instr, 25)) return false;   // bits 25..23 = 1xx: no existe
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
