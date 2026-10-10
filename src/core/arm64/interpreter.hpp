#pragma once
#include <algorithm>
#include <cstring>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <string>
#include "common/types.hpp"
#include "common/bit_utils.hpp"
#include "cpu_state.hpp"
#include "jit_dynarmic.hpp"
#include "memory.hpp"

namespace NeXo2::Core {

class Interpreter;

// Una instruccion ya decodificada (ver interpreter_fast.cpp).
// 'fn' es la funcion que la ejecuta; el resto son sus operandos ya extraidos,
// asi no hay que volver a mirar bits cada vez que se ejecuta.
struct DecodedInstr {
    using Handler = bool (*)(Interpreter&, const DecodedInstr&);
    Handler fn = nullptr;   // al principio: Interpreter::DecodeAndRun
    u32 raw = 0;            // la instruccion original
    u8  rd = 0, rn = 0, rm = 0, ra = 0;
    u8  sf = 0;             // 1 = registros X (64 bits)
    u8  a = 0, b = 0, c = 0; // campos extra, segun la instruccion
    u64 imm = 0;            // inmediato, mascara o destino del salto
    u64 imm2 = 0;           // segunda mascara o direccion de retorno
};

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
//   interpreter_simd_ldst.cpp -> lecturas/escrituras de registros SIMD (ldr q0, stp q0, ld1...)
//   interpreter_fp.cpp       -> coma flotante escalar (fadd d0, fcmp, scvtf, fmov...)
//   interpreter_simd.cpp     -> SIMD vectorial (dup, movi, cmeq, addp, ext, uzp1...)
class Interpreter {
public:
    // Frecuencia del contador del sistema en Switch 2 (ver
    // docs/05-switch2-system/compatibility-mode.md). La Switch 1 usaba 19.2 MHz.
    static constexpr u64 TICK_FREQUENCY = 31'250'000;
    // Velocidad de la CPU emulada: la de la consola en modo portatil, 998,4 MHz (Cortex-A78C).
    // Contamos una instruccion por ciclo, asi que el contador del sistema avanza
    // TICK_FREQUENCY / CPU_FREQUENCY ticks por instruccion (625 / 19968, unas 32 instrucciones
    // por tick). Con el reloj ligado al tiempo real, el programa ve una CPU de 998,4 MHz.
    static constexpr u64 CPU_FREQUENCY = 998'400'000;
    static constexpr u64 TICKS_NUM = 625, TICKS_DEN = 19968;   // TICK_FREQUENCY / CPU_FREQUENCY simplificado
    static_assert(TICK_FREQUENCY * TICKS_DEN == CPU_FREQUENCY * TICKS_NUM, "fraccion de ticks incorrecta");

    // Se llama cuando el programa ejecuta "SVC #imm" (llamada al kernel).
    // En la Fase 4 aqui ira el kernel HLE. Por defecto solo se registra en el log.
    using SvcHandler = std::function<void(u32 imm, CPUState& state)>;

    // 'core_index': nucleo emulado de esta CPU (el JIT lo usa en el monitor exclusivo comun)
    explicit Interpreter(Memory& memory, u32 core_index = 0);
    ~Interpreter();
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    void Reset();

    // Ejecuta una instruccion. Devuelve false si la CPU se ha parado
    // (instruccion no implementada, BRK o Halt()).
    bool Step();

    // Ejecuta hasta 'max_steps' instrucciones o hasta que la CPU se pare.
    // Devuelve cuantas instrucciones se ejecutaron.
    u64 Run(u64 max_steps);

    // Cache de instrucciones decodificadas (activada por defecto). Desactivarla
    // vuelve al camino lento de siempre: sirve para comparar los dos en los tests.
    void SetDecodeCacheEnabled(bool enabled) { m_cacheEnabled = enabled; ClearDecodeCache(); }
    bool IsDecodeCacheEnabled() const { return m_cacheEnabled; }
    void ClearDecodeCache() { m_decodeCache.clear(); }
    size_t DecodedPages() const { return m_decodeCache.size(); }

    // Para la CPU (por ejemplo desde un SvcHandler).
    void Halt(const std::string& reason);
    bool IsHalted() const { return m_halted; }
    // Quita la parada para poder seguir ejecutando (por ejemplo tras cambiar el PC).
    void Resume() { m_halted = false; m_haltReason.clear(); m_attention = true; }
    const std::string& GetHaltReason() const { return m_haltReason; }

    void SetSvcHandler(SvcHandler handler) { m_svcHandler = std::move(handler); }

    // Termina Run() despues de la instruccion actual, SIN parar la CPU. Lo usa el
    // kernel cuando un hilo se bloquea (espera, duerme...) para cambiar a otro hilo.
    // Se puede llamar desde otro hilo del PC (el kernel parando un nucleo).
    void RequestStop() { m_stopRequested = true; m_attention = true; if (m_jit) NotifyJitStop(); }
    // Adelanta el reloj (CNTPCT) sin ejecutar nada: todos los hilos estan dormidos.
    // (Con un reloj externo no hace nada: ese reloj ya es la hora real.)
    void AddTicks(u64 ticks) {
        if (m_clock) return;
        const u64 target = GetTicks() + ticks;   // menos instrucciones que lleguen a 'target' ticks
        m_instructionCount = std::max(m_instructionCount, (target * TICKS_DEN + TICKS_NUM - 1) / TICKS_NUM);
    }
    // Contador del sistema (CNTPCT_EL0, svcGetSystemTick), a TICK_FREQUENCY
    u64 GetTicks() const { return m_clock ? m_clock(m_clockCtx) : TicksFor(m_instructionCount); }
    // Reloj externo: con varios nucleos en hilos del PC cada uno ejecuta a su ritmo, asi que
    // el contador sale de un reloj comun (la hora real, ver Kernel). nullptr = instrucciones.
    using ClockFn = u64 (*)(const void* ctx);
    void SetClock(ClockFn fn, const void* ctx) { m_clock = fn; m_clockCtx = ctx; }
    static u64 TicksFor(u64 instructions) { return instructions / TICKS_DEN * TICKS_NUM + instructions % TICKS_DEN * TICKS_NUM / TICKS_DEN; }
    // Al cambiar de hilo se pierde la reserva de LDXR (como en la CPU real).
    void ClearExclusive() { m_exclusiveValid = false; if (m_jit) ClearJitExclusive(); }

    // --- JIT (dynarmic, jit_dynarmic.cpp) ---
    // Con el JIT activado, Run() traduce el codigo a x86-64 en vez de interpretarlo.
    // Todo lo demas (GetState(), SVC, Halt...) funciona igual.
    static bool JitAvailable();                        // NeXo se compilo con dynarmic
    void SetJitEnabled(bool enabled);
    bool IsJitEnabled() const { return m_jitEnabled; }
    const JitBackend* GetJit() const { return m_jit.get(); }
    // Valor inicial para las CPUs que se creen despues (los tests lo cambian con NEXO2_TEST_JIT)
    static void SetDefaultJitEnabled(bool enabled);

    // Numero de nucleo emulado de esta CPU (0..5)
    u32  CoreIndex() const { return m_coreIndex; }

    u64 GetInstructionCount() const { return m_instructionCount; }

    CPUState&       GetState()       { return m_state; }
    const CPUState& GetState() const { return m_state; }

private:
    friend class JitImpl;     // el JIT usa el estado, el SVC y el interprete para lo que no sabe
    void NotifyJitStop();
    void ClearJitExclusive();
    std::unique_ptr<JitBackend> m_jit;
    bool m_jitEnabled = false;

    friend struct FastOps;   // las funciones rapidas de interpreter_fast.cpp
    friend struct SimdOps;   // las instrucciones SIMD de interpreter_simd.cpp

    // --- Cache de instrucciones decodificadas ---
    // Una pagina de 4 KB tiene 1024 instrucciones: una entrada por cada una.
    struct CachePage { std::array<DecodedInstr, 1024> entries; };
    u64  RunPlain(u64 max_steps);        // sin cache: fetch + decode + execute cada vez
    u64  RunCached(u64 max_steps);       // con cache
    CachePage* GetCachePage(u64 page_base);
    void HandleCodeWrites();             // el programa escribio en paginas de codigo
    static void Decode(u32 raw, u64 pc, DecodedInstr& out);  // interpreter_fast.cpp
    static bool DecodeAndRun(Interpreter& it, const DecodedInstr& entry);

    // --- Decodificacion por grupos (cada uno en su .cpp) ---
    // Devuelven false si la instruccion no esta implementada.
    bool Execute(u32 instr);
    bool ExecDataProcImm(u32 instr);
    bool ExecDataProcReg(u32 instr);
    bool DpLogicalShifted(u32 instr);
    bool DpAddSubShifted(u32 instr);
    bool DpAddSubExtended(u32 instr);
    bool DpAddSubCarry(u32 instr);
    bool DpCondCompare(u32 instr);
    bool DpCondSelect(u32 instr);
    bool DpSource12(u32 instr);
    bool DpSource3(u32 instr);
    bool ExecBranchSystem(u32 instr);
    bool ExecLoadStore(u32 instr);

    bool ExecSystemRegister(u32 instr);
    bool ExecSimdLoadStore(u32 instr);
    bool ExecSimdFp(u32 instr);        // reparte entre coma flotante y SIMD vectorial
    bool ExecFloatingPoint(u32 instr);
    bool ExecSimdVector(u32 instr);
    bool ExecCrypto(u32 instr);        // AES y SHA1/SHA256 (interpreter_crypto.cpp)

    // Registros SIMD: escribir un escalar (s0, d0...) pone a cero el resto del registro
    V128& Vreg(unsigned n) { return m_state.v[n & 31]; }
    void  SetVScalar(unsigned n, u64 value, unsigned bytes);
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
    // "Hay que mirar algo": la CPU se paro o el programa escribio en codigo.
    // Asi el bucle rapido solo comprueba una variable por instruccion.
    // (Atomicas: otro hilo del PC puede activarlas: escrituras en codigo, parar el nucleo.)
    std::atomic<bool> m_attention{false};
    std::atomic<bool> m_stopRequested{false};   // ver RequestStop()
    std::string m_haltReason;

    // Monitor exclusivo para LDXR/STXR. Se guarda lo que leyo LDXR y STXR solo escribe si
    // la memoria sigue igual (compare-and-swap atomico): funciona con varios nucleos a la vez.
    bool m_exclusiveValid = false;
    u64  m_exclusiveAddr  = 0;
    u64  m_exclusiveValue[2] = {0, 0};   // valor(es) leidos (LDXP lee dos)

    ClockFn     m_clock = nullptr;
    const void* m_clockCtx = nullptr;
    u32         m_coreIndex = 0;

    bool m_cacheEnabled = true;
    std::unordered_map<u64, std::unique_ptr<CachePage>> m_decodeCache;  // base de pagina -> cache
    u64  m_cacheGeneration = ~0ull;   // Memory::Generation() de cuando se lleno
    u64  m_seenCodeWrites = 0;        // Memory::CodeWriteCount() ya atendidos
};


// ============================================================================
//  Funciones pequenas que se usan en casi todas las instrucciones. Estan aqui
//  (inline) para que el compilador las meta dentro de quien las llama: llamar
//  a una funcion por cada registro leido costaba mas que la operacion en si.
// ============================================================================

inline bool Interpreter::Execute(u32 instr) {
    const u32 op0 = Common::Bits(instr, 25, 4);

    if ((op0 & 0b1110) == 0b1000) return ExecDataProcImm(instr);   // 100x
    if ((op0 & 0b1110) == 0b1010) return ExecBranchSystem(instr);  // 101x
    if ((op0 & 0b0101) == 0b0100) return ExecLoadStore(instr);     // x1x0
    if ((op0 & 0b0111) == 0b0101) return ExecDataProcReg(instr);   // x101
    if ((op0 & 0b0111) == 0b0111) return ExecSimdFp(instr);        // x111
    return false;
}

inline u64 Interpreter::X(unsigned n, bool sf) const {
    const u64 v = m_state.x[n & 31];           // x[31] siempre vale 0 = XZR
    return sf ? v : (v & 0xFFFFFFFFu);
}

inline void Interpreter::SetX(unsigned n, u64 value, bool sf) {
    // Escribir un registro W pone a cero los 32 bits altos del X.
    // Escribir en XZR no hace nada: se escribe en x[31] y se vuelve a poner a 0
    // (dos escrituras sin "if" son mas rapidas que una comprobacion).
    m_state.x[n & 31] = sf ? value : (value & 0xFFFFFFFFu);
    m_state.x[31] = 0;
}

inline u64 Interpreter::XorSP(unsigned n, bool sf) const {
    const u64 v = (n < 31) ? m_state.x[n] : m_state.sp; // 31 = SP
    return sf ? v : (v & 0xFFFFFFFFu);
}

inline void Interpreter::SetXorSP(unsigned n, u64 value, bool sf) {
    if (!sf) value &= 0xFFFFFFFFu;
    if (n < 31) m_state.x[n] = value;
    else        m_state.sp   = value;
}

inline u64 Interpreter::AddWithCarry(u64 a, u64 b, bool carry_in, bool sf, bool set_flags) {
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

inline void Interpreter::SetLogicFlags(u64 result, bool sf) {
    m_state.flags.n = sf ? ((result >> 63) & 1) : ((result >> 31) & 1);
    m_state.flags.z = sf ? (result == 0) : (static_cast<u32>(result) == 0);
    m_state.flags.c = false;
    m_state.flags.v = false;
}

inline bool Interpreter::ConditionHolds(unsigned cond) const {
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

inline u64 Interpreter::ShiftReg(u64 value, unsigned type, unsigned amount, bool sf) const {
    using namespace NeXo2::Common;
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

inline u64 Interpreter::ReadMemory(u64 addr, unsigned size_bytes) {
    switch (size_bytes) {
        case 1:  return m_memory.Read<u8>(addr);
        case 2:  return m_memory.Read<u16>(addr);
        case 4:  return m_memory.Read<u32>(addr);
        default: return m_memory.Read<u64>(addr);
    }
}

inline void Interpreter::WriteMemory(u64 addr, u64 value, unsigned size_bytes) {
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
