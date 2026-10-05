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

    // 1) FETCH: 32 bits desde memoria en PC (todas las instrucciones ARM64 miden 4 bytes).
    const u32 instr = m_memory.Read<u32>(m_state.pc);

    // Por defecto la siguiente instruccion es PC + 4. Los saltos cambian m_nextPc.
    m_nextPc = m_state.pc + 4;

    // 2) DECODE + EXECUTE
    if (!Execute(instr)) {
        LogUnimplemented(instr);
        return false; // el PC se queda apuntando a la instruccion que fallo
    }

    // 3) Avanzar
    m_state.pc = m_nextPc;
    ++m_instructionCount;
    return !m_halted;
}

u64 Interpreter::Run(u64 max_steps) {
    u64 executed = 0;
    while (executed < max_steps && Step()) ++executed;
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
bool Interpreter::Execute(u32 instr) {
    const u32 op0 = Bits(instr, 25, 4);

    if ((op0 & 0b1110) == 0b1000) return ExecDataProcImm(instr);   // 100x
    if ((op0 & 0b1110) == 0b1010) return ExecBranchSystem(instr);  // 101x
    if ((op0 & 0b0101) == 0b0100) return ExecLoadStore(instr);     // x1x0
    if ((op0 & 0b0111) == 0b0101) return ExecDataProcReg(instr);   // x101
    // x111 = SIMD / coma flotante: pendiente.
    return false;
}

// ============================================================================
//  Registros
// ============================================================================

u64 Interpreter::X(unsigned n, bool sf) const {
    const u64 v = (n < 31) ? m_state.x[n] : 0; // 31 = XZR
    return sf ? v : (v & 0xFFFFFFFFu);
}

void Interpreter::SetX(unsigned n, u64 value, bool sf) {
    // Escribir un registro W pone a cero los 32 bits altos del X.
    if (n < 31) m_state.x[n] = sf ? value : (value & 0xFFFFFFFFu);
}

u64 Interpreter::XorSP(unsigned n, bool sf) const {
    const u64 v = (n < 31) ? m_state.x[n] : m_state.sp; // 31 = SP
    return sf ? v : (v & 0xFFFFFFFFu);
}

void Interpreter::SetXorSP(unsigned n, u64 value, bool sf) {
    if (!sf) value &= 0xFFFFFFFFu;
    if (n < 31) m_state.x[n] = value;
    else        m_state.sp   = value;
}

// ============================================================================
//  Aritmetica y flags
// ============================================================================

u64 Interpreter::AddWithCarry(u64 a, u64 b, bool carry_in, bool sf, bool set_flags) {
    u64 result;
    if (sf) {
        const u64 partial = a + b;
        result = partial + (carry_in ? 1 : 0);
        if (set_flags) {
            m_state.flags.n = (result >> 63) & 1;
            m_state.flags.z = (result == 0);
            // Acarreo: la suma sin signo no cabe en 64 bits.
            m_state.flags.c = (partial < a) || (result < partial);
            // Desbordamiento: a y b tienen el mismo signo pero el resultado no.
            m_state.flags.v = ((~(a ^ b) & (a ^ result)) >> 63) & 1;
        }
    } else {
        const u32 a32 = static_cast<u32>(a), b32 = static_cast<u32>(b);
        const u64 wide = u64(a32) + u64(b32) + (carry_in ? 1 : 0);
        const u32 r32 = static_cast<u32>(wide);
        result = r32;
        if (set_flags) {
            m_state.flags.n = (r32 >> 31) & 1;
            m_state.flags.z = (r32 == 0);
            m_state.flags.c = (wide >> 32) & 1;
            m_state.flags.v = ((~(a32 ^ b32) & (a32 ^ r32)) >> 31) & 1;
        }
    }
    return result;
}

void Interpreter::SetLogicFlags(u64 result, bool sf) {
    m_state.flags.n = sf ? ((result >> 63) & 1) : ((result >> 31) & 1);
    m_state.flags.z = sf ? (result == 0) : (static_cast<u32>(result) == 0);
    m_state.flags.c = false;
    m_state.flags.v = false;
}

// Condiciones de B.cond, CSEL, CCMP... (tabla "Condition codes" del manual).
bool Interpreter::ConditionHolds(unsigned cond) const {
    const auto& f = m_state.flags;
    bool result;
    switch (cond >> 1) {
        case 0: result = f.z;                    break; // EQ / NE
        case 1: result = f.c;                    break; // CS(HS) / CC(LO)
        case 2: result = f.n;                    break; // MI / PL
        case 3: result = f.v;                    break; // VS / VC
        case 4: result = f.c && !f.z;            break; // HI / LS
        case 5: result = (f.n == f.v);           break; // GE / LT
        case 6: result = (f.n == f.v) && !f.z;   break; // GT / LE
        default: return true;                           // AL / NV (siempre)
    }
    // El bit 0 invierte la condicion (EQ->NE, GE->LT...), salvo en AL/NV.
    return (cond & 1) ? !result : result;
}

u64 Interpreter::ShiftReg(u64 value, unsigned type, unsigned amount, bool sf) const {
    const unsigned size = sf ? 64 : 32;
    value &= Ones(size);
    amount %= size;
    switch (type) {
        case 0: return (value << amount) & Ones(size);                       // LSL
        case 1: return value >> amount;                                      // LSR
        case 2: return static_cast<u64>(SignExtend(value, size) >> amount)   // ASR
                       & Ones(size);
        default: return RotateRight(value, amount, size);                    // ROR
    }
}

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

// ============================================================================
//  Memoria
// ============================================================================

u64 Interpreter::ReadMemory(u64 addr, unsigned size_bytes) {
    switch (size_bytes) {
        case 1:  return m_memory.Read<u8>(addr);
        case 2:  return m_memory.Read<u16>(addr);
        case 4:  return m_memory.Read<u32>(addr);
        default: return m_memory.Read<u64>(addr);
    }
}

void Interpreter::WriteMemory(u64 addr, u64 value, unsigned size_bytes) {
    // Una escritura normal rompe una reserva LDXR sobre esa direccion.
    if (m_exclusiveValid && m_exclusiveAddr == addr) m_exclusiveValid = false;
    switch (size_bytes) {
        case 1:  m_memory.Write<u8>(addr, static_cast<u8>(value));   break;
        case 2:  m_memory.Write<u16>(addr, static_cast<u16>(value)); break;
        case 4:  m_memory.Write<u32>(addr, static_cast<u32>(value)); break;
        default: m_memory.Write<u64>(addr, value);                   break;
    }
}

} // namespace NeXo2::Core
