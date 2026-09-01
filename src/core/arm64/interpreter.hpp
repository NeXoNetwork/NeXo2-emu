#pragma once
#include "common/types.hpp"
#include "cpu_state.hpp"
#include "memory.hpp"

namespace NeXo2::Core {

// Interprete de la CPU ARM64 (Cortex-A78C).
// Decodifica y ejecuta un subconjunto real del set ARMv8: MOVZ/MOVN/MOVK,
// ADD/SUB inmediato y NOP. El resto se registra como "no implementado".
class Interpreter {
public:
    explicit Interpreter(Memory& memory) : m_memory(memory) {
        Reset();
    }

    void Reset();
    void Step(); // Fetch -> Decode -> Execute de una instruccion

    CPUState&       GetState()       { return m_state; }
    const CPUState& GetState() const { return m_state; }

private:
    void Execute(u32 instr);

    // Acceso a registros X0..X30. El indice 31 es XZR (lee 0, escribir lo descarta).
    u64  ReadReg(unsigned n) const;
    void WriteReg(unsigned n, u64 value);

    CPUState m_state;
    Memory&  m_memory;
};

} // namespace NeXo2::Core
