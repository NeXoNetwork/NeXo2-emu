#include "interpreter.hpp"
#include "common/logger.hpp"
#include <cstdio>

namespace NeXo2::Core {

void Interpreter::Reset() {
    m_state.Reset();
    // Dirección de arranque de ejemplo (se ajustará al cargar un binario real).
    m_state.pc = 0x80000000;
    Common::Logger::Log(Common::Logger::Level::Info, "[CPU] Reset: PC = 0x80000000");
}

void Interpreter::Step() {
    // 1) FETCH: leemos la instruccion de 32 bits desde memoria en PC.
    const u32 instr = m_memory.Read<u32>(m_state.pc);
    // 2) DECODE + EXECUTE
    Execute(instr);
    // 3) Avanzamos al siguiente opcode (ARM64 = 4 bytes fijos).
    m_state.pc += 4;
}

void Interpreter::Execute(u32 instr) {
    // TODO (Fase 2): decodificador real ARMv8.2-A.
    // De momento sólo distinguimos NOP y "desconocido" para tener el esqueleto.
    if (instr == 0xD503201F) { // NOP
        return;
    }
    // Evitamos spamear al leer memoria vacía (0x00000000).
    if (instr != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[CPU] Opcode no implementado: 0x%08X", instr);
        Common::Logger::Log(Common::Logger::Level::Warning, buf);
    }
}

} // namespace NeXo2::Core
