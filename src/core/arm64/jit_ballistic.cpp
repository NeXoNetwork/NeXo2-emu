#include "jit_ballistic.hpp"
#include "common/logger.hpp"

namespace NeXo2::Core {

BallisticJit::BallisticJit() {
    bal_allocator_init_default(&m_allocator);
    bal_logger_init_default(&m_logger);
    m_logger.min_level = BAL_LOG_LEVEL_WARN;

    if (bal_engine_init(&m_allocator, &m_engine, m_logger) == BAL_SUCCESS) {
        m_ready = true;
        Common::Logger::Log(Common::Logger::Level::Info,
                            "[JIT] Ballistic inicializado (front-end IR, sin backend todavia).");
    } else {
        Common::Logger::Log(Common::Logger::Level::Error,
                            "[JIT] Fallo al inicializar Ballistic.");
    }
}

BallisticJit::~BallisticJit() {
    if (m_ready) {
        bal_engine_destroy(&m_allocator, &m_engine);
    }
}

int BallisticJit::TranslateFlat(const uint32_t* code, std::size_t count) {
    if (!m_ready || code == nullptr || count == 0) {
        return -1;
    }

    // Ballistic lee el codigo a traves de una interfaz de memoria. Para un
    // bloque contiguo usamos la interfaz "plana" (guest addr == offset).
    bal_memory_interface_t interface{};
    const std::size_t bytes = count * sizeof(uint32_t);
    if (bal_memory_init_flat(&m_allocator, &interface,
                             const_cast<uint32_t*>(code), bytes, m_logger) != BAL_SUCCESS) {
        return -1;
    }

    const bal_error_t rc = bal_engine_translate(&m_engine, &interface, code, bytes);
    const int ir_count = (rc == BAL_SUCCESS) ? static_cast<int>(m_engine.instruction_count) : -1;

    bal_memory_destroy_flat(&m_allocator, &interface);
    bal_engine_reset(&m_engine); // dejamos el motor listo para la proxima unidad
    return ir_count;
}

} // namespace NeXo2::Core
