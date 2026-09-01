#pragma once
#include "common/types.hpp"
#include "cpu_state.hpp"
#include "memory.hpp"

namespace NeXo2::Core {

// Intérprete de la CPU ARM64 (Cortex-A78C).
// De momento sólo realiza el ciclo Fetch/Decode/Execute a nivel esqueleto.
class Interpreter {
public:
    explicit Interpreter(Memory& memory) : m_memory(memory) {
        Reset();
    }

    void Reset();
    void Step(); // Ejecuta una instrucción

    CPUState&       GetState()       { return m_state; }
    const CPUState& GetState() const { return m_state; }

private:
    void Execute(u32 instr);

    CPUState m_state;
    Memory&  m_memory;
};

} // namespace NeXo2::Core
