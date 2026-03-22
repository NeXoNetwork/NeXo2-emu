#pragma once
#include "cpu_state.hpp"
#include "../memory/memory.hpp"

namespace NeXo2::Core {

class Interpreter {
public:
    explicit Interpreter(Memory& mem) : m_memory(mem) {
        m_state.Reset();
    }

    // Ejecuta el ciclo de instrucción
    void Step();

    // Acceso al estado para debugging
    CPUState& GetState() { return m_state; }

private:
    void Execute(uint32_t instr);

    CPUState m_state;
    Memory& m_memory;
};

} // namespace NeXo2::Core