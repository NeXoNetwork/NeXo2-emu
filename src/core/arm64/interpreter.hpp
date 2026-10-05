#pragma once
#include <functional>
#include <string>
#include "common/types.hpp"
#include "cpu_state.hpp"
#include "memory.hpp"

namespace NeXo2::Core {

// Interprete de la CPU ARM64 (Cortex-A78C), solo enteros por ahora.
//
// Ejecuta una instruccion cada vez: Fetch -> Decode -> Execute.
// El codigo esta repartido por grupos de instrucciones, igual que en el
// manual de ARM (ver docs/06-resources/external-links.md):
//   interpreter.cpp          -> bucle principal, flags, condiciones, helpers
//   interpreter_dp_imm.cpp   -> procesado de datos con inmediato (MOV, ADD #, AND #, LSL #...)
//   interpreter_dp_reg.cpp   -> procesado de datos con registros (ADD, CSEL, MUL, UDIV...)
//   interpreter_branch.cpp   -> saltos y sistema (B, BL, RET, CBZ, SVC, MRS...)
//   interpreter_ldst.cpp     -> lecturas/escrituras en memoria (LDR, STR, LDP, STP...)
class Interpreter {
public:
    // Frecuencia del contador del sistema en Switch 2 (ver
    // docs/05-switch2-system/compatibility-mode.md). La Switch 1 usaba 19.2 MHz.
    static constexpr u64 TICK_FREQUENCY = 31'250'000;

    // Se llama cuando el programa ejecuta "SVC #imm" (llamada al kernel).
    // En la Fase 4 aqui ira el kernel HLE. Por defecto solo se registra en el log.
    using SvcHandler = std::function<void(u32 imm, CPUState& state)>;

    explicit Interpreter(Memory& memory) : m_memory(memory) {
        Reset();
    }

    void Reset();

    // Ejecuta una instruccion. Devuelve false si la CPU se ha parado
    // (instruccion no implementada, BRK o Halt()).
    bool Step();

    // Ejecuta hasta 'max_steps' instrucciones o hasta que la CPU se pare.
    // Devuelve cuantas instrucciones se ejecutaron.
    u64 Run(u64 max_steps);

    // Para la CPU (por ejemplo desde un SvcHandler).
    void Halt(const std::string& reason);
    bool IsHalted() const { return m_halted; }
    // Quita la parada para poder seguir ejecutando (por ejemplo tras cambiar el PC).
    void Resume() { m_halted = false; m_haltReason.clear(); }
    const std::string& GetHaltReason() const { return m_haltReason; }

    void SetSvcHandler(SvcHandler handler) { m_svcHandler = std::move(handler); }

    u64 GetInstructionCount() const { return m_instructionCount; }

    CPUState&       GetState()       { return m_state; }
    const CPUState& GetState() const { return m_state; }

private:
    // --- Decodificacion por grupos (cada uno en su .cpp) ---
    // Devuelven false si la instruccion no esta implementada.
    bool Execute(u32 instr);
    bool ExecDataProcImm(u32 instr);
    bool ExecDataProcReg(u32 instr);
    bool ExecBranchSystem(u32 instr);
    bool ExecLoadStore(u32 instr);

    bool ExecSystemRegister(u32 instr);
    bool ExecLoadStorePair(u32 instr);
    bool ExecLoadStoreExclusive(u32 instr);

    // --- Registros ---
    // En ARM64 el numero 31 significa XZR (cero) o SP segun la instruccion.
    //   X / SetX       -> 31 = XZR (lee 0, escribir no hace nada)
    //   XorSP / SetXorSP -> 31 = SP
    // 'sf' = true para registros X (64 bits), false para W (32 bits).
    u64  X(unsigned n, bool sf = true) const;
    void SetX(unsigned n, u64 value, bool sf = true);
    u64  XorSP(unsigned n, bool sf = true) const;
    void SetXorSP(unsigned n, u64 value, bool sf = true);

    // --- Aritmetica y condiciones ---
    // Suma a + b + carry y (si set_flags) actualiza NZCV, como AddWithCarry() del manual.
    u64  AddWithCarry(u64 a, u64 b, bool carry_in, bool sf, bool set_flags);
    void SetLogicFlags(u64 result, bool sf); // N y Z segun resultado, C = V = 0
    bool ConditionHolds(unsigned cond) const; // EQ, NE, LT, GE...
    u64  ShiftReg(u64 value, unsigned type, unsigned amount, bool sf) const; // LSL/LSR/ASR/ROR
    u64  ExtendReg(unsigned reg, unsigned option, unsigned shift, bool sf) const; // UXTB..SXTX

    // --- Memoria ---
    u64  ReadMemory(u64 addr, unsigned size_bytes);
    void WriteMemory(u64 addr, u64 value, unsigned size_bytes);

    void LogUnimplemented(u32 instr);

    CPUState m_state;
    Memory&  m_memory;
    SvcHandler m_svcHandler;

    u64  m_nextPc = 0;           // a donde ira el PC tras la instruccion actual
    u64  m_instructionCount = 0; // tambien hace de contador CNTPCT_EL0 (provisional)
    bool m_halted = false;
    std::string m_haltReason;

    // Monitor exclusivo para LDXR/STXR (mutex y atomicos). Version de un solo nucleo.
    bool m_exclusiveValid = false;
    u64  m_exclusiveAddr  = 0;
};

} // namespace NeXo2::Core
