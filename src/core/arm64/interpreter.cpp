#include "interpreter.hpp"
#include "common/bit_utils.hpp"
#include "common/logger.hpp"
#include <cstdio>

namespace NeXo2::Core {

using namespace NeXo2::Common;

// ============================================================================
//  Bucle principal
// ============================================================================

void Interpreter::Reset() {
    m_state.Reset();
    // Direccion de arranque de ejemplo (donde main.cpp carga el programa).
    m_state.pc = 0x80000000;
    m_nextPc = m_state.pc;
    m_instructionCount = 0;
    m_halted = false;
    m_haltReason.clear();
    m_exclusiveValid = false;
    Logger::Log(Logger::Level::Info, "[CPU] Reset: PC = 0x80000000");
}

bool Interpreter::Step() {
    if (m_halted) return false;
    return Run(1) == 1 && !m_halted;
}

// Bucle principal. Para ir rapido guarda un puntero a la pagina de codigo actual
// (4 KB): mientras el PC siga dentro de ella, leer la instruccion es un acceso
// directo a array, sin buscar la pagina en la tabla cada vez.
u64 Interpreter::Run(u64 max_steps) {
    u64 executed = 0;
    u64 page_base = ~0ull;               // direccion de la pagina guardada
    const u8* page = nullptr;            // se pide de nuevo en cada Run(): la memoria solo
                                         // se borra (Memory::Clear) fuera de Run, al cargar otro programa

    while (executed < max_steps && !m_halted) {
        const u64 pc = m_state.pc;

        // 1) FETCH: 32 bits desde memoria en PC (todas las instrucciones ARM64 miden 4 bytes).
        u32 instr;
        if (page && (pc & ~(Memory::PAGE_SIZE - 1)) == page_base) {
            std::memcpy(&instr, page + (pc & (Memory::PAGE_SIZE - 1)), 4);
        } else {
            page_base = pc & ~(Memory::PAGE_SIZE - 1);
            page = m_memory.PagePointer(pc);
            instr = m_memory.Read<u32>(pc);   // tambien vale si la pagina no existe (lee 0)
        }

        // Por defecto la siguiente instruccion es PC + 4. Los saltos cambian m_nextPc.
        m_nextPc = pc + 4;

        // 2) DECODE + EXECUTE
        if (!Execute(instr)) {
            LogUnimplemented(instr);
            break;                           // el PC se queda en la instruccion que fallo
        }

        // 3) Avanzar
        m_state.pc = m_nextPc;
        ++m_instructionCount;
        ++executed;

    }
    return executed;
}

void Interpreter::Halt(const std::string& reason) {
    m_halted = true;
    m_haltReason = reason;
    Logger::Log(Logger::Level::Info, "[CPU] Parada: " + reason);
}

void Interpreter::LogUnimplemented(u32 instr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Opcode no implementado 0x%08X en PC=0x%016llX",
                  instr, static_cast<unsigned long long>(m_state.pc));
    Logger::Log(Logger::Level::Warning, std::string("[CPU] ") + buf);
    Halt(buf);
}

// Primer nivel de decodificacion: los bits 28..25 ("op0") dicen a que grupo
// pertenece la instruccion (tabla "Top-level encodings" del manual de ARM).

// ============================================================================
//  Registros
// ============================================================================





// ============================================================================
//  Aritmetica y flags
// ============================================================================



// Condiciones de B.cond, CSEL, CCMP... (tabla "Condition codes" del manual).


u64 Interpreter::ExtendReg(unsigned reg, unsigned option, unsigned shift, bool sf) const {
    u64 v = X(reg, true);
    switch (option) {
        case 0: v = v & 0xFF;                                        break; // UXTB
        case 1: v = v & 0xFFFF;                                      break; // UXTH
        case 2: v = v & 0xFFFFFFFF;                                  break; // UXTW
        case 3:                                                      break; // UXTX
        case 4: v = static_cast<u64>(SignExtend(v, 8));              break; // SXTB
        case 5: v = static_cast<u64>(SignExtend(v, 16));             break; // SXTH
        case 6: v = static_cast<u64>(SignExtend(v, 32));             break; // SXTW
        default:                                                     break; // SXTX
    }
    v <<= shift;
    return sf ? v : (v & 0xFFFFFFFFu);
}

void Interpreter::SetVScalar(unsigned n, u64 value, unsigned bytes) {
    V128& v = Vreg(n);
    v = V128{};
    v.Set(0, bytes, value);
}

// ============================================================================
//  Memoria
// ============================================================================



} // namespace NeXo2::Core
