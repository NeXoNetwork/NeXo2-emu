// Procesado de datos con inmediato (bits 28..26 = 100).
// Instrucciones: ADR, ADRP, ADD/SUB #imm, AND/ORR/EOR #imm, MOVZ/MOVN/MOVK,
// SBFM/BFM/UBFM (de aqui salen LSL/LSR/ASR #, UXTB, SXTW...) y EXTR (ROR #).
#include "interpreter.hpp"
#include "common/bit_utils.hpp"

namespace NeXo2::Core {

using namespace NeXo2::Common;

bool Interpreter::ExecDataProcImm(u32 instr) {
    const u32  op  = Bits(instr, 23, 3);  // subgrupo
    const bool sf  = Bit(instr, 31);      // 1 = registros X (64 bits), 0 = W (32 bits)
    const u32  rd  = Bits(instr, 0, 5);
    const u32  rn  = Bits(instr, 5, 5);
    const unsigned datasize = sf ? 64 : 32;

    switch (op) {
    // ------------------------------------------------------------------
    // ADR / ADRP: calcular direcciones relativas al PC
    // ------------------------------------------------------------------
    case 0b000:
    case 0b001: {
        const u64 immlo = Bits(instr, 29, 2);
        const u64 immhi = Bits(instr, 5, 19);
        const s64 imm   = SignExtend((immhi << 2) | immlo, 21);
        if (Bit(instr, 31) == 0) {
            SetX(rd, m_state.pc + imm);                                // ADR
        } else {
            SetX(rd, (m_state.pc & ~0xFFFULL) + (static_cast<u64>(imm) << 12)); // ADRP (paginas de 4 KB)
        }
        return true;
    }

    // ------------------------------------------------------------------
    // ADD / ADDS / SUB / SUBS (inmediato).  CMP x, #n  = SUBS xzr, x, #n
    // ------------------------------------------------------------------
    case 0b010: {
        const bool is_sub    = Bit(instr, 30);
        const bool set_flags = Bit(instr, 29);
        u64 imm = Bits(instr, 10, 12);
        if (Bit(instr, 22)) imm <<= 12;

        const u64 a = XorSP(rn, sf); // aqui el registro 31 es SP ("mov x0, sp")
        const u64 result = is_sub ? AddWithCarry(a, ~imm, true, sf, set_flags)
                                  : AddWithCarry(a, imm, false, sf, set_flags);
        if (set_flags) SetX(rd, result, sf);     // ADDS/SUBS: 31 = XZR
        else           SetXorSP(rd, result, sf); // ADD/SUB:   31 = SP
        return true;
    }

    // ------------------------------------------------------------------
    // AND / ORR / EOR / ANDS (inmediato logico, codificado como mascara)
    // ------------------------------------------------------------------
    case 0b100: {
        const u32 opc = Bits(instr, 29, 2);
        const u32 n   = Bit(instr, 22);
        if (!sf && n) return false; // combinacion invalida
        u64 wmask, tmask;
        if (!DecodeBitMasks(n, Bits(instr, 10, 6), Bits(instr, 16, 6), true, datasize, wmask, tmask))
            return false;

        const u64 a = X(rn, sf);
        u64 result;
        switch (opc) {
            case 0b00: result = a & wmask; break; // AND
            case 0b01: result = a | wmask; break; // ORR  (MOV x0, #mascara)
            case 0b10: result = a ^ wmask; break; // EOR
            default:   result = a & wmask; break; // ANDS (TST)
        }
        if (opc == 0b11) {
            SetLogicFlags(result, sf);
            SetX(rd, result, sf);
        } else {
            SetXorSP(rd, result, sf);
        }
        return true;
    }

    // ------------------------------------------------------------------
    // MOVN / MOVZ / MOVK (move wide): cargar constantes de 16 en 16 bits
    // ------------------------------------------------------------------
    case 0b101: {
        const u32 opc   = Bits(instr, 29, 2);   // 00=MOVN, 10=MOVZ, 11=MOVK
        const u32 hw    = Bits(instr, 21, 2);   // desplazamiento / 16
        if (!sf && hw >= 2) return false;
        const unsigned shift = hw * 16;
        const u64 imm = u64(Bits(instr, 5, 16)) << shift;
        switch (opc) {
            case 0b00: SetX(rd, ~imm, sf); return true;            // MOVN
            case 0b10: SetX(rd, imm, sf);  return true;            // MOVZ
            case 0b11: {                                           // MOVK
                u64 v = X(rd, sf);
                v &= ~(0xFFFFULL << shift);
                SetX(rd, v | imm, sf);
                return true;
            }
            default: return false;
        }
    }

    // ------------------------------------------------------------------
    // SBFM / BFM / UBFM (bitfield). Alias muy usados por los compiladores:
    //   LSL/LSR/ASR #n, UBFX, SBFX, BFI, UXTB, UXTH, SXTB, SXTH, SXTW
    // ------------------------------------------------------------------
    case 0b110: {
        const u32 opc  = Bits(instr, 29, 2);    // 00=SBFM, 01=BFM, 10=UBFM
        const u32 n    = Bit(instr, 22);
        const u32 immr = Bits(instr, 16, 6);
        const u32 imms = Bits(instr, 10, 6);
        if (opc == 0b11 || n != (sf ? 1u : 0u)) return false;
        if (!sf && (immr >= 32 || imms >= 32)) return false;
        u64 wmask, tmask;
        if (!DecodeBitMasks(n, imms, immr, false, datasize, wmask, tmask)) return false;

        const u64 src = X(rn, sf);
        const u64 rotated = RotateRight(src, immr, datasize);
        u64 result;
        if (opc == 0b01) {                      // BFM: inserta bits en el destino
            const u64 dst = X(rd, sf);
            const u64 bot = (dst & ~wmask) | (rotated & wmask);
            result = (dst & ~tmask) | (bot & tmask);
        } else {
            const u64 bot = rotated & wmask;
            // SBFM rellena con el bit de signo del campo; UBFM rellena con ceros.
            const u64 top = (opc == 0b00 && ((src >> imms) & 1)) ? Ones(datasize) : 0;
            result = (top & ~tmask) | (bot & tmask);
        }
        SetX(rd, result & Ones(datasize), sf);
        return true;
    }

    // ------------------------------------------------------------------
    // EXTR (y su alias ROR #n): concatena Rn:Rm y extrae 'datasize' bits
    // ------------------------------------------------------------------
    case 0b111: {
        if (Bits(instr, 29, 2) != 0 || Bit(instr, 21) != 0) return false;
        if ((Bit(instr, 22) != 0) != sf) return false;   // N tiene que ser igual a sf
        const u32 rm  = Bits(instr, 16, 5);
        const u32 lsb = Bits(instr, 10, 6);
        if (!sf && lsb >= 32) return false;
        const u64 hi = X(rn, sf), lo = X(rm, sf);
        const u64 result = (lsb == 0) ? lo
                         : ((lo >> lsb) | (hi << (datasize - lsb))) & Ones(datasize);
        SetX(rd, result, sf);
        return true;
    }

    default:
        return false;
    }
}

} // namespace NeXo2::Core
