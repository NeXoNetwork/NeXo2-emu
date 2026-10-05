// Procesado de datos con registros (bits 27..25 = 101).
// Instrucciones: AND/ORR/EOR/BIC... (registro), ADD/SUB (registro y extendido),
// ADC/SBC, CCMP/CCMN, CSEL/CSINC/CSINV/CSNEG, UDIV/SDIV, LSLV/LSRV/ASRV/RORV,
// RBIT/REV/CLZ/CLS, MADD/MSUB (MUL), SMULH/UMULH, SMADDL/UMADDL...
#include "interpreter.hpp"
#include "common/bit_utils.hpp"

namespace NeXo2::Core {

using namespace NeXo2::Common;

// Reparte segun op1 (bit 28) y op2 (bits 24..21). Cada familia esta en su propia
// funcion pequena: asi la instruccion mas comun no paga el coste (registros que
// guardar, variables) de todas las demas.
bool Interpreter::ExecDataProcReg(u32 instr) {
    const u32 op1 = Bit(instr, 28);
    const u32 op2 = Bits(instr, 21, 4);

    if (op1 == 0) {
        if ((op2 & 0b1000) == 0)      return DpLogicalShifted(instr);
        if ((op2 & 0b1001) == 0b1000) return DpAddSubShifted(instr);
        return DpAddSubExtended(instr);
    }
    switch (op2) {
        case 0b0000: return DpAddSubCarry(instr);
        case 0b0010: return DpCondCompare(instr);
        case 0b0100: return DpCondSelect(instr);
        case 0b0110: return DpSource12(instr);
        default:     return (op2 & 0b1000) ? DpSource3(instr) : false;
    }
}

// AND, BIC, ORR, ORN, EOR, EON, ANDS, BICS con registro desplazado ("MOV x0, x1" = ORR x0, xzr, x1)
bool Interpreter::DpLogicalShifted(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rd = Bits(instr, 0, 5);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const unsigned datasize = sf ? 64 : 32;
    const u32 opc   = Bits(instr, 29, 2);
    const u32 shift = Bits(instr, 22, 2);
    const u32 imm6  = Bits(instr, 10, 6);
    if (!sf && imm6 >= 32) return false;
    u64 b = ShiftReg(X(rm, sf), shift, imm6, sf);
    if (Bit(instr, 21)) b = ~b & Ones(datasize); // N=1: versiones negadas (BIC, ORN, EON)
    const u64 a = X(rn, sf);
    u64 result;
    switch (opc) {
        case 0b00: result = a & b; break;
        case 0b01: result = a | b; break;
        case 0b10: result = a ^ b; break;
        default:   result = a & b; SetLogicFlags(result, sf); break; // ANDS/BICS (TST)
    }
    SetX(rd, result, sf);
    return true;
}

// ADD/SUB con registro desplazado.  CMP x0, x1 = SUBS xzr, x0, x1
bool Interpreter::DpAddSubShifted(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rd = Bits(instr, 0, 5);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const bool is_sub    = Bit(instr, 30);
    const bool set_flags = Bit(instr, 29);
    const u32 shift = Bits(instr, 22, 2);
    const u32 imm6  = Bits(instr, 10, 6);
    if (shift == 3 || (!sf && imm6 >= 32)) return false;
    const u64 a = X(rn, sf);
    const u64 b = ShiftReg(X(rm, sf), shift, imm6, sf);
    const u64 result = is_sub ? AddWithCarry(a, ~b, true, sf, set_flags)
                              : AddWithCarry(a, b, false, sf, set_flags);
    SetX(rd, result, sf);
    return true;
}

// ADD/SUB con registro extendido: "add x0, sp, w1, uxtw #2"
bool Interpreter::DpAddSubExtended(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rd = Bits(instr, 0, 5);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const bool is_sub    = Bit(instr, 30);
    const bool set_flags = Bit(instr, 29);
    if (Bits(instr, 22, 2) != 0) return false;
    const u32 option = Bits(instr, 13, 3);
    const u32 imm3   = Bits(instr, 10, 3);
    if (imm3 > 4) return false;
    const u64 a = XorSP(rn, sf);
    const u64 b = ExtendReg(rm, option, imm3, sf);
    const u64 result = is_sub ? AddWithCarry(a, ~b, true, sf, set_flags)
                              : AddWithCarry(a, b, false, sf, set_flags);
    if (set_flags) SetX(rd, result, sf);
    else           SetXorSP(rd, result, sf);
    return true;
}

// ADC / ADCS / SBC / SBCS: suma/resta usando el acarreo (C)
bool Interpreter::DpAddSubCarry(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rd = Bits(instr, 0, 5);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const u32 op0 = Bit(instr, 30);
    if (Bits(instr, 10, 6) != 0) return false;
    const bool set_flags = Bit(instr, 29);
    const u64 a = X(rn, sf);
    u64 b = X(rm, sf);
    if (op0) b = ~b; // SBC = a + ~b + C
    SetX(rd, AddWithCarry(a, b, m_state.flags.c, sf, set_flags), sf);
    return true;
}

// CCMP / CCMN: compara solo si se cumple la condicion; si no, NZCV = #nzcv
bool Interpreter::DpCondCompare(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const u32 op0 = Bit(instr, 30);
    if (!Bit(instr, 29) || Bit(instr, 10) || Bit(instr, 4)) return false;
    const u32 cond = Bits(instr, 12, 4);
    if (ConditionHolds(cond)) {
        const u64 a = X(rn, sf);
        const u64 b = Bit(instr, 11) ? u64(rm)   // forma inmediata: imm5 va donde Rm
                                     : X(rm, sf);
        if (op0) AddWithCarry(a, ~b, true, sf, true);  // CCMP
        else     AddWithCarry(a, b, false, sf, true);  // CCMN
    } else {
        m_state.SetNZCV(u64(Bits(instr, 0, 4)) << 28);
    }
    return true;
}

// CSEL / CSINC / CSINV / CSNEG.  Alias: CSET, CSETM, CINC, CNEG...
bool Interpreter::DpCondSelect(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rd = Bits(instr, 0, 5);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const unsigned datasize = sf ? 64 : 32;
    const u32 op0 = Bit(instr, 30);
    if (Bit(instr, 29)) return false;
    const u32 cond = Bits(instr, 12, 4);
    const u32 op2b = Bits(instr, 10, 2);
    if (op2b > 1 || Bit(instr, 29)) return false;   // op2 = 1x: no valida
    u64 result;
    if (ConditionHolds(cond)) {
        result = X(rn, sf);
    } else {
        const u64 b = X(rm, sf);
        if      (op0 == 0 && op2b == 0) result = b;          // CSEL
        else if (op0 == 0 && op2b == 1) result = b + 1;      // CSINC
        else if (op0 == 1 && op2b == 0) result = ~b;         // CSINV
        else if (op0 == 1 && op2b == 1) result = 0 - b;      // CSNEG
        else return false;
    }
    SetX(rd, result & Ones(datasize), sf);
    return true;
}

// Datos con 2 fuentes (op0=0: UDIV, SDIV, LSLV...) o 1 fuente (op0=1: RBIT, REV, CLZ...)
bool Interpreter::DpSource12(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rd = Bits(instr, 0, 5);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const unsigned datasize = sf ? 64 : 32;
    const u32 op0 = Bit(instr, 30);
    if (Bit(instr, 29)) return false;
    const u32 opcode = Bits(instr, 10, 6);

    if (op0 == 0) {
        const u64 a = X(rn, sf), b = X(rm, sf);
        u64 result;
        switch (opcode) {
            case 0b000010: // UDIV (dividir entre 0 da 0 en ARM, no excepcion)
                result = (b == 0) ? 0 : a / b;
                break;
            case 0b000011: { // SDIV
                const s64 sa = SignExtend(a, datasize), sb = SignExtend(b, datasize);
                if (sb == 0)                                   result = 0;
                else if (sb == -1)                             result = 0 - u64(sa); // evita INT_MIN / -1
                else                                           result = u64(sa / sb);
                break;
            }
            case 0b001000: result = ShiftReg(a, 0, unsigned(b % datasize), sf); break; // LSLV
            case 0b001001: result = ShiftReg(a, 1, unsigned(b % datasize), sf); break; // LSRV
            case 0b001010: result = ShiftReg(a, 2, unsigned(b % datasize), sf); break; // ASRV
            case 0b001011: result = ShiftReg(a, 3, unsigned(b % datasize), sf); break; // RORV
            case 0b010000: case 0b010001: case 0b010010: case 0b010011:   // CRC32B/H/W/X
            case 0b010100: case 0b010101: case 0b010110: case 0b010111: { // CRC32CB/H/W/X
                // CRC "reflejado" sin invertir al principio ni al final (eso lo hace el programa)
                const unsigned size = 8u << (opcode & 3);
                if ((size == 64) != sf) return false;
                const u32 poly = (opcode & 4) ? 0x82F63B78u : 0xEDB88320u;
                u64 crc = X(rn, false);
                const u64 data = X(rm, sf) & Ones(size);
                for (unsigned i = 0; i < size; ++i) {
                    const u64 bit = (crc ^ (data >> i)) & 1;
                    crc = (crc >> 1) ^ (bit ? poly : 0);
                }
                SetX(rd, crc & 0xFFFFFFFF, false);
                return true;
            }
            default: return false;
        }
        SetX(rd, result & Ones(datasize), sf);
        return true;
    }

    // 1 fuente
    const u32 opcode2 = Bits(instr, 16, 5);
    if (opcode2 == 0b00001) {
        // PACIA/AUTIA/XPACI... (Pointer Authentication). Como no firmamos
        // punteros, el valor no cambia: equivalen a NOP.
        //   opcode 0-7: PACIA..AUTDB;  8-15: las versiones "Z" (Rn = 11111);
        //   16-17: XPACI/XPACD (Rn = 11111). El resto no existe.
        const u32 op = Bits(instr, 10, 6);
        if (!sf || op > 17 || (op >= 8 && rn != 31)) return false;
        return true;
    }
    if (opcode2 != 0) return false;
    const u64 a = X(rn, sf);
    u64 result = 0;
    switch (opcode) {
        case 0b000000: // RBIT: invierte el orden de los bits
            for (unsigned i = 0; i < datasize; ++i)
                if ((a >> i) & 1) result |= 1ULL << (datasize - 1 - i);
            break;
        case 0b000001: // REV16: invierte bytes dentro de cada media palabra
            for (unsigned i = 0; i < datasize; i += 16) {
                const u64 h = (a >> i) & 0xFFFF;
                result |= (((h & 0xFF) << 8) | (h >> 8)) << i;
            }
            break;
        case 0b000010: // REV32 (X) / REV (W): invierte bytes de cada palabra de 32 bits
        case 0b000011: { // REV (X): invierte los 8 bytes
            if (opcode == 0b000011 && !sf) return false;
            const unsigned container = (opcode == 0b000011) ? 64 : 32;
            for (unsigned base = 0; base < datasize; base += container)
                for (unsigned i = 0; i < container; i += 8) {
                    const u64 byte = (a >> (base + i)) & 0xFF;
                    result |= byte << (base + container - 8 - i);
                }
            break;
        }
        case 0b000100: { // CLZ: cuenta ceros por la izquierda
            unsigned count = 0;
            for (int i = int(datasize) - 1; i >= 0 && !((a >> i) & 1); --i) ++count;
            result = count;
            break;
        }
        case 0b000101: { // CLS: cuenta bits iguales al de signo (sin contarlo)
            const u64 sign = (a >> (datasize - 1)) & 1;
            unsigned count = 0;
            for (int i = int(datasize) - 2; i >= 0 && ((a >> i) & 1) == sign; --i) ++count;
            result = count;
            break;
        }
        default: return false;
    }
    SetX(rd, result, sf);
    return true;
}

// Datos con 3 fuentes: MADD/MSUB (MUL, MNEG), SMADDL, UMADDL, SMULH, UMULH
bool Interpreter::DpSource3(u32 instr) {
    const bool sf = Bit(instr, 31);
    const u32  rd = Bits(instr, 0, 5);
    const u32  rn = Bits(instr, 5, 5);
    const u32  rm = Bits(instr, 16, 5);
    const unsigned datasize = sf ? 64 : 32;
    if (Bits(instr, 29, 2) != 0) return false;
    const u32  op31 = Bits(instr, 21, 3);
    const bool o0   = Bit(instr, 15);
    const u32  ra   = Bits(instr, 10, 5);

    switch (op31) {
        case 0b000: { // MADD / MSUB:  Rd = Ra +/- Rn*Rm
            const u64 prod = X(rn, sf) * X(rm, sf);
            SetX(rd, (o0 ? X(ra, sf) - prod : X(ra, sf) + prod) & Ones(datasize), sf);
            return true;
        }
        case 0b001:   // SMADDL / SMSUBL: 32x32 con signo -> 64
        case 0b101: { // UMADDL / UMSUBL: 32x32 sin signo -> 64
            if (!sf) return false;
            const u64 a = (op31 == 0b001) ? u64(SignExtend(X(rn), 32)) : (X(rn) & 0xFFFFFFFF);
            const u64 b = (op31 == 0b001) ? u64(SignExtend(X(rm), 32)) : (X(rm) & 0xFFFFFFFF);
            const u64 prod = a * b;
            SetX(rd, o0 ? X(ra) - prod : X(ra) + prod);
            return true;
        }
        case 0b010:   // SMULH: parte alta de 64x64 con signo
            if (!sf || o0) return false;
            SetX(rd, MulHighSigned(s64(X(rn)), s64(X(rm))));
            return true;
        case 0b110:   // UMULH: parte alta de 64x64 sin signo
            if (!sf || o0) return false;
            SetX(rd, MulHighUnsigned(X(rn), X(rm)));
            return true;
        default:
            return false;
    }
}

} // namespace NeXo2::Core
