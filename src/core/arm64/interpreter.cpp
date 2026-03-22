#include "interpreter.hpp"
#include <iostream>
#include <iomanip>

namespace NeXo2::Core {

void Interpreter::Step() {
    // 1. Fetch: Leer instrucción de 32 bits desde el PC actual
    uint32_t instr = m_memory.Read<uint32_t>(m_state.pc);

    // 2. Execute: Procesar la instrucción
    Execute(instr);

    // 3. Increment PC: En ARM64 las instrucciones siempre miden 4 bytes
    m_state.pc += 4;
}

void Interpreter::Execute(uint32_t instr) {
    // Obtenemos el registro de destino (bits 0-4)
    uint32_t rd = instr & 0x1F;

    // Ejemplo: Decodificar un MOV inmediato (0x52800000)
    // Esto es una simplificación para testear que el motor gira
    if ((instr & 0x7F800000) == 0x52800000) {
        uint32_t imm = (instr >> 5) & 0xFFFF;
        m_state.x[rd] = imm;
        
        std::cout << "[CPU] Executed: MOV X" << std::dec << rd 
                  << ", #0x" << std::hex << imm << std::endl;
    } else {
        std::cout << "[CPU] Unknown Instruction: 0x" << std::hex << instr << std::endl;
    }
}

} // namespace NeXo2::Core