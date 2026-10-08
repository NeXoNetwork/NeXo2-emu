// Saltos, excepciones y sistema (bits 28..26 = 101).
// Instrucciones: B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR, BLR, RET (y RETAA/RETAB),
// SVC, BRK, NOP y demas HINT (incluidas PACIASP/AUTIASP), barreras, MRS/MSR.
#include "interpreter.hpp"
#include "fp_ops.hpp"
#include "common/bit_utils.hpp"
#include "common/logger.hpp"
#include <cstdio>

namespace NeXo2::Core {

using namespace NeXo2::Common;

// Codigos de registro de sistema = bits 19..5 de MRS/MSR (o0:op1:CRn:CRm:op2).
namespace SysReg {
    constexpr u32 NZCV        = 0x5A10; // S3_3_C4_C2_0
    constexpr u32 FPCR        = 0x5A20; // S3_3_C4_C4_0
    constexpr u32 FPSR        = 0x5A21; // S3_3_C4_C4_1
    constexpr u32 TPIDR_EL0   = 0x5E82; // S3_3_C13_C0_2
    constexpr u32 TPIDRRO_EL0 = 0x5E83; // S3_3_C13_C0_3
    constexpr u32 CNTFRQ_EL0  = 0x5F00; // S3_3_C14_C0_0
    constexpr u32 CNTPCT_EL0  = 0x5F01; // S3_3_C14_C0_1
    constexpr u32 CNTVCT_EL0  = 0x5F02; // S3_3_C14_C0_2
    constexpr u32 CTR_EL0     = 0x5801; // S3_3_C0_C0_1: tamanos de linea de cache
    constexpr u32 DCZID_EL0   = 0x5807; // S3_3_C0_C0_7: bloque de "dc zva"
}

// Lineas de cache de 64 bytes (como el Cortex-A78C): IminLine = DminLine = 4 (2^4 palabras)
constexpr u64 CTR_EL0_VALUE   = 0x8444C004;
constexpr u64 DC_ZVA_BLOCK    = 64;      // DCZID_EL0 = 4 -> 2^4 palabras de 4 bytes

bool Interpreter::ExecBranchSystem(u32 instr) {
    const u64 pc = m_state.pc;

    // ------------------------------------------------------------------
    // B / BL: salto incondicional relativo (+-128 MB). BL guarda el retorno en X30.
    // ------------------------------------------------------------------
    if (Bits(instr, 26, 5) == 0b00101) {
        const s64 offset = SignExtend(Bits(instr, 0, 26), 26) * 4;
        if (Bit(instr, 31)) m_state.x[30] = pc + 4; // BL = "llamar a funcion"
        m_nextPc = pc + offset;
        return true;
    }

    // ------------------------------------------------------------------
    // CBZ / CBNZ: salta si el registro es (o no es) cero
    // ------------------------------------------------------------------
    if (Bits(instr, 25, 6) == 0b011010) {
        const bool sf = Bit(instr, 31);
        const bool is_nz = Bit(instr, 24);
        const s64 offset = SignExtend(Bits(instr, 5, 19), 19) * 4;
        const bool is_zero = X(Bits(instr, 0, 5), sf) == 0;
        if (is_zero != is_nz) m_nextPc = pc + offset;
        return true;
    }

    // ------------------------------------------------------------------
    // TBZ / TBNZ: salta si un bit concreto del registro es 0 (o 1)
    // ------------------------------------------------------------------
    if (Bits(instr, 25, 6) == 0b011011) {
        const unsigned bit = (Bit(instr, 31) << 5) | Bits(instr, 19, 5);
        const bool is_nz = Bit(instr, 24);
        const s64 offset = SignExtend(Bits(instr, 5, 14), 14) * 4;
        const bool bit_set = (X(Bits(instr, 0, 5)) >> bit) & 1;
        if (bit_set == is_nz) m_nextPc = pc + offset;
        return true;
    }

    // ------------------------------------------------------------------
    // B.cond: salto condicional (B.EQ, B.NE, B.LT...) segun los flags NZCV
    // ------------------------------------------------------------------
    if (Bits(instr, 24, 8) == 0b01010100 && Bit(instr, 4) == 0) {
        const s64 offset = SignExtend(Bits(instr, 5, 19), 19) * 4;
        if (ConditionHolds(Bits(instr, 0, 4))) m_nextPc = pc + offset;
        return true;
    }

    // ------------------------------------------------------------------
    // SVC #imm: llamada al kernel (aqui entrara el HLE de Horizon en la Fase 4)
    // BRK #imm: punto de ruptura -> paramos la CPU
    // ------------------------------------------------------------------
    if ((instr & 0xFFE0001Fu) == 0xD4000001u) {
        const u32 imm = Bits(instr, 5, 16);
        if (m_svcHandler) {
            m_svcHandler(imm, m_state);
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "[CPU] SVC 0x%02X (sin kernel HLE todavia)", imm);
            Logger::Log(Logger::Level::Warning, buf);
        }
        return true;
    }
    if ((instr & 0xFFE0001Fu) == 0xD4200000u) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "BRK #0x%X", Bits(instr, 5, 16));
        m_nextPc = pc; // como un punto de ruptura: el PC se queda en el BRK
        Halt(buf);
        return true;
    }

    // ------------------------------------------------------------------
    // HINT: NOP, YIELD, WFE, BTI, PACIASP, AUTIASP...
    // Las de Pointer Authentication viven aqui; sin firma real son NOP.
    // ------------------------------------------------------------------
    if ((instr & 0xFFFFF01Fu) == 0xD503201Fu) return true;

    // Barreras (DSB, DMB, ISB) y CLREX: con un solo nucleo no hacen falta.
    if ((instr & 0xFFFFF01Fu) == 0xD503301Fu) {
        if (Bits(instr, 5, 3) == 0b010) m_exclusiveValid = false; // CLREX
        return true;
    }

    // ------------------------------------------------------------------
    // SYS: mantenimiento de cache (dc cvac, dc civac, ic ivau...). Sin caches
    // emuladas son NOP, salvo "dc zva", que pone a cero un bloque de 64 bytes
    // (memset la usa para limpiar memoria rapido).
    // ------------------------------------------------------------------
    if ((instr & 0xFFF80000u) == 0xD5080000u) {
        if ((instr & 0xFFFFFFE0u) == 0xD50B7420u) {                      // DC ZVA, Xt
            const u64 addr = X(Bits(instr, 0, 5)) & ~(DC_ZVA_BLOCK - 1);
            static const u8 zeros[DC_ZVA_BLOCK] = {};
            m_memory.WriteBytes(addr, zeros, DC_ZVA_BLOCK);
        }
        return true;
    }

    // ------------------------------------------------------------------
    // MRS / MSR: leer/escribir registros de sistema
    // ------------------------------------------------------------------
    if ((instr & 0xFFD00000u) == 0xD5100000u) return ExecSystemRegister(instr);

    // ------------------------------------------------------------------
    // BR / BLR / RET: saltos a una direccion guardada en un registro
    // ------------------------------------------------------------------
    {
        const u32 rn = Bits(instr, 5, 5);
        switch (instr & 0xFFFFFC1Fu) {
            case 0xD61F0000u: m_nextPc = X(rn); return true;                     // BR
            case 0xD63F0000u: { const u64 t = X(rn);                             // BLR
                                m_state.x[30] = pc + 4; m_nextPc = t; return true; }
            case 0xD65F0000u: m_nextPc = X(rn); return true;                     // RET (Xn, normalmente X30)
            default: break;
        }
        // RETAA / RETAB: RET con comprobacion PAC. Sin firma real = RET normal.
        if (instr == 0xD65F0BFFu || instr == 0xD65F0FFFu) {
            m_nextPc = m_state.x[30];
            return true;
        }
    }

    return false;
}


bool Interpreter::ExecSystemRegister(u32 instr) {
    const bool is_read = Bit(instr, 21);   // 1 = MRS (leer), 0 = MSR (escribir)
    const u32  reg     = Bits(instr, 5, 15);
    const u32  rt      = Bits(instr, 0, 5);

    if (is_read) {
        u64 value;
        switch (reg) {
            case SysReg::NZCV:        value = m_state.GetNZCV();   break;
            case SysReg::FPCR:        value = m_state.fpcr;        break;
            case SysReg::FPSR:        FP::FoldHostFlags(m_state.fpsr); value = m_state.fpsr; break;  // + flags de la FPU del PC
            case SysReg::TPIDR_EL0:   value = m_state.tpidr_el0;   break;
            case SysReg::TPIDRRO_EL0: value = m_state.tpidrro_el0; break;
            case SysReg::CNTFRQ_EL0:  value = TICK_FREQUENCY;      break;
            // INSTRUCTIONS_PER_TICK instrucciones = 1 tick (la interfaz lo ata a la hora real)
            case SysReg::CNTPCT_EL0:
            case SysReg::CNTVCT_EL0:  value = m_instructionCount / INSTRUCTIONS_PER_TICK;  break;
            case SysReg::CTR_EL0:     value = CTR_EL0_VALUE;       break;
            case SysReg::DCZID_EL0:   value = 4;                   break;
            default: return false;
        }
        SetX(rt, value);
        return true;
    }

    const u64 value = X(rt);
    switch (reg) {
        case SysReg::NZCV:      m_state.SetNZCV(value);    return true;
        case SysReg::FPCR:      m_state.fpcr = value;      return true;
        case SysReg::FPSR:      FP::ClearHostFlags(); m_state.fpsr = value; return true;
        case SysReg::TPIDR_EL0: m_state.tpidr_el0 = value; return true;
        default: return false; // TPIDRRO_EL0 es solo lectura para el programa
    }
}

} // namespace NeXo2::Core
