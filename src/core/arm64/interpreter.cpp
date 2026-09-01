#include "interpreter.hpp"
#include "common/logger.hpp"
#include <cstdio>

namespace NeXo2::Core {

u64 Interpreter::ReadReg(unsigned n) const {
    // En estas instrucciones, el registro 31 es el registro cero (XZR).
    return (n < 31) ? m_state.x[n] : 0;
}

void Interpreter::WriteReg(unsigned n, u64 value) {
    // Escribir en XZR (31) no tiene efecto.
    if (n < 31) m_state.x[n] = value;
}

void Interpreter::Reset() {
    m_state.Reset();
    // Direccion de arranque de ejemplo (donde main.cpp carga el programa).
    m_state.pc = 0x80000000;
    Common::Logger::Log(Common::Logger::Level::Info, "[CPU] Reset: PC = 0x80000000");
}

void Interpreter::Step() {
    // 1) FETCH: 32 bits desde memoria en PC.
    const u32 instr = m_memory.Read<u32>(m_state.pc);
    // 2) DECODE + EXECUTE
    Execute(instr);
    // 3) Siguiente opcode (ARM64 = 4 bytes fijos).
    //    (Cuando implementemos saltos, esto se movera dentro de Execute.)
    m_state.pc += 4;
}

void Interpreter::Execute(u32 instr) {
    const unsigned rd = instr & 0x1F;

    // --- NOP ---
    if (instr == 0xD503201Fu) return;

    // --- Move wide: MOVN / MOVZ / MOVK (bits 28:23 == 100101) ---
    if (((instr >> 23) & 0x3F) == 0x25u) {
        const unsigned opc     = (instr >> 29) & 0x3u;   // 00=MOVN, 10=MOVZ, 11=MOVK
        const unsigned hw      = (instr >> 21) & 0x3u;   // desplazamiento / 16
        const u64      imm16   = (instr >> 5) & 0xFFFFu;
        const unsigned shift   = hw * 16u;
        const u64      shifted = imm16 << shift;
        switch (opc) {
            case 0x2u: WriteReg(rd, shifted);  return;   // MOVZ: Rd = imm << shift
            case 0x0u: WriteReg(rd, ~shifted); return;   // MOVN: Rd = ~(imm << shift)
            case 0x3u: {                                 // MOVK: inserta imm, conserva el resto
                u64 v = ReadReg(rd);
                v &= ~(static_cast<u64>(0xFFFFu) << shift);
                v |= shifted;
                WriteReg(rd, v);
                return;
            }
            default: break;
        }
    }

    // --- ADD/SUB inmediato (bits 28:24 == 10001) ---
    if (((instr >> 24) & 0x1Fu) == 0x11u) {
        const unsigned op    = (instr >> 30) & 0x1u;     // 0=ADD, 1=SUB
        const unsigned sh    = (instr >> 22) & 0x1u;     // si 1, imm12 <<= 12
        u64            imm12 = (instr >> 10) & 0xFFFu;
        if (sh) imm12 <<= 12;
        const unsigned rn = (instr >> 5) & 0x1Fu;
        const u64      a  = ReadReg(rn);
        WriteReg(rd, op ? (a - imm12) : (a + imm12));
        return;
    }

    // --- No implementada (evitamos spamear con el 0x00000000 de memoria vacia) ---
    if (instr != 0u) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[CPU] Opcode no implementado: 0x%08X", instr);
        Common::Logger::Log(Common::Logger::Level::Warning, buf);
    }
}

} // namespace NeXo2::Core
