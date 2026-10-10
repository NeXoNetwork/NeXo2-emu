#include "interpreter.hpp"
#include "common/bit_utils.hpp"
#include "common/logger.hpp"
#include <cstdio>

namespace NeXo2::Core {

using namespace NeXo2::Common;

// ============================================================================
//  Bucle principal
// ============================================================================

namespace {
bool g_defaultJit = false;   // ver SetDefaultJitEnabled()
}

Interpreter::Interpreter(Memory& memory, u32 core_index) : m_memory(memory), m_coreIndex(core_index) {
    m_memory.AddCodeWriteFlag(&m_attention);
    Reset();
    if (g_defaultJit) SetJitEnabled(true);
}
Interpreter::~Interpreter() {
    m_jit.reset();
    m_memory.RemoveCodeWriteFlag(&m_attention);
}

bool Interpreter::JitAvailable() { return JitBackend::Available(); }
void Interpreter::SetDefaultJitEnabled(bool enabled) { g_defaultJit = enabled && JitAvailable(); }

void Interpreter::SetJitEnabled(bool enabled) {
    enabled = enabled && JitAvailable();
    if (enabled == m_jitEnabled) return;
    m_jitEnabled = enabled;
    m_decodeCache.clear();
    if (enabled && !m_jit) m_jit = std::make_unique<JitBackend>(*this, m_memory);  // se crea al usarlo
    else if (m_jit) m_jit->ClearCache();
}
void Interpreter::NotifyJitStop() { m_jit->RequestHalt(); }
void Interpreter::ClearJitExclusive() { m_jit->ClearExclusive(); }

void Interpreter::Reset() {
    m_state.Reset();
    // Direccion de arranque de ejemplo (donde main.cpp carga el programa).
    m_state.pc = 0x80000000;
    m_nextPc = m_state.pc;
    m_instructionCount = 0;
    m_halted = false;
    m_haltReason.clear();
    m_exclusiveValid = false;
    m_stopRequested = false;
    m_decodeCache.clear();
    if (m_jit) { m_jit->ClearCache(); m_jit->ClearExclusive(); }
    Logger::Log(Logger::Level::Info, "[CPU] Reset: PC = 0x80000000");
}

bool Interpreter::Step() {
    if (m_halted) return false;
    return Run(1) == 1 && !m_halted;
}

u64 Interpreter::Run(u64 max_steps) {
    m_state.x[31] = 0;   // por si alguien lo cambio desde fuera (GetState())
    m_stopRequested = false;  // una peticion que llego justo al final del Run() anterior ya no vale
    if (m_jitEnabled) return m_halted ? 0 : m_jit->Run(max_steps);
    return m_cacheEnabled ? RunCached(max_steps) : RunPlain(max_steps);
}

// Bucle sin cache. Guarda un puntero a la pagina de codigo actual (4 KB): mientras
// el PC siga dentro de ella, leer la instruccion es un acceso directo a array.
u64 Interpreter::RunPlain(u64 max_steps) {
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
        if (m_stopRequested) { m_stopRequested = false; break; }
    }
    return executed;
}

// ============================================================================
//  Cache de instrucciones decodificadas
// ============================================================================
//
// La primera vez que se ejecuta una direccion, Decode() mira sus bits una sola vez
// y guarda la funcion que la ejecuta y sus operandos. Las siguientes veces se
// salta toda la decodificacion. Si el programa escribe en una pagina de codigo,
// Memory lo apunta y aqui se tira la cache de esa pagina.

Interpreter::CachePage* Interpreter::GetCachePage(u64 page_base) {
    auto& slot = m_decodeCache[page_base];
    if (!slot) {
        slot = std::make_unique<CachePage>();
        // Todas las entradas empiezan apuntando a "decodificame y ejecutame"
        for (auto& e : slot->entries) e.fn = &DecodeAndRun;
        m_memory.MarkCode(page_base);           // avisame si escriben aqui
    }
    return slot.get();
}

void Interpreter::HandleCodeWrites() {
    std::array<u64, 8> pages;
    size_t count = 0;
    if (m_memory.TakeCodeWrites(m_seenCodeWrites, pages, count)) {
        for (size_t i = 0; i < count; ++i) m_decodeCache.erase(pages[i]);
    } else {
        m_decodeCache.clear();                  // demasiadas paginas: empezar de cero
    }
}

u64 Interpreter::RunCached(u64 max_steps) {
    // Otro programa cargado (Memory::Clear): nada de lo guardado vale
    if (m_cacheGeneration != m_memory.Generation()) {
        m_decodeCache.clear();
        m_cacheGeneration = m_memory.Generation();
        m_seenCodeWrites = m_memory.CodeWriteCount();
    }

    const u64 start = m_instructionCount;
    const u64 end = start + max_steps;           // el contador de instrucciones hace de contador del bucle
    u64 page_base = ~0ull;
    CachePage* page = nullptr;
    m_attention = true;                          // revisar todo antes de la primera instruccion

    while (m_instructionCount < end) {
        // Una sola comprobacion por instruccion: Halt() y las escrituras en codigo la activan
        if (m_attention) [[unlikely]] {
            if (m_halted) break;
            if (m_stopRequested) { m_stopRequested = false; m_attention = false; break; }
            m_attention = false;
            if (m_memory.CodeWriteCount() != m_seenCodeWrites) HandleCodeWrites();
            page_base = ~0ull;                   // la pagina actual puede haberse tirado
        }

        const u64 pc = m_state.pc;
        // Cambio de pagina o PC desalineado (los 2 bits bajos tambien cuentan en la comparacion)
        if ((pc & (~(Memory::PAGE_SIZE - 1) | 3)) != page_base) [[unlikely]] {
            if (pc & 3) {                        // PC desalineado: camino lento
                RunPlain(1);
                break;
            }
            page_base = pc & ~(Memory::PAGE_SIZE - 1);
            page = GetCachePage(page_base);
        }

        const DecodedInstr& d = page->entries[(pc & (Memory::PAGE_SIZE - 1)) >> 2];
        m_nextPc = pc + 4;
        if (!d.fn(*this, d)) [[unlikely]] {
            LogUnimplemented(d.raw);
            break;
        }
        m_state.pc = m_nextPc;
        ++m_instructionCount;
    }
    return m_instructionCount - start;
}

// Primera vez que se ejecuta una entrada: decodificar, guardar y ejecutar.
// (LogUnimplemented usa d.raw, asi que hay que guardarlo aunque falle.)
bool Interpreter::DecodeAndRun(Interpreter& it, const DecodedInstr& entry) {
    const u64 pc = it.m_state.pc;
    DecodedInstr& d = const_cast<DecodedInstr&>(entry);
    Decode(it.m_memory.Read<u32>(pc), pc, d);
    return d.fn(it, d);
}

void Interpreter::Halt(const std::string& reason) {
    m_halted = true;
    m_attention = true;
    m_haltReason = reason;
    if (m_jit) m_jit->RequestHalt();
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
