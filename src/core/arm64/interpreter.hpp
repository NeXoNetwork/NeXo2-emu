#pragma once
#include "cpu_state.hpp"
#include "../memory/memory.hpp"

namespace NeXo2::Core {

class Interpreter {
public:
    // Constructor: Necesita una referencia a la memoria para funcionar
    explicit Interpreter(Memory& mem) : m_memory(mem) {
        m_state.Reset();
    }

    // Ejecuta un ciclo: Leer -> Ejecutar -> Avanzar
    void Step();
    
    // Decodifica la instrucción binaria
    void Execute(uint32_t instr);

    // Acceso al estado para el Main
    CPUState& GetState() { return m_state; }

private:
    CPUState m_state;   // El estado de los registros
    Memory& m_memory;   // La referencia a la RAM
};

} // namespace NeXo2::Core