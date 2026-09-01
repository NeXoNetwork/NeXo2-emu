#pragma once
#include <cstddef>
#include <cstdint>

// Las cabeceras de Ballistic son C puro y (casi ninguna) trae guardas para
// C++, asi que las envolvemos en extern "C" para enlazar bien desde C++.
extern "C" {
#include "bal_engine.h"
#include "bal_memory.h"
#include "bal_logging.h"
#include "bal_errors.h"
#include "bal_types.h"
}

namespace NeXo2::Core {

// Envoltorio del motor JIT Ballistic.
//
// AVISO: hoy Ballistic solo traduce ARM64 -> su IR interna (front-end).
// Todavia NO tiene backend, por lo que NO ejecuta codigo: produce IR.
// Este wrapper nos deja alimentarle codigo ARM y ver cuanta IR genera.
class BallisticJit {
public:
    BallisticJit();
    ~BallisticJit();

    BallisticJit(const BallisticJit&)            = delete;
    BallisticJit& operator=(const BallisticJit&) = delete;

    bool IsReady() const { return m_ready; }

    // Traduce 'count' instrucciones ARM64 (32 bits c/u) a la IR de Ballistic.
    // Devuelve el numero de instrucciones IR generadas, o -1 si hay error.
    // 'code' debe estar alineado a 16 bytes (lo exige bal_memory_init_flat).
    int TranslateFlat(const uint32_t* code, std::size_t count);

private:
    bal_allocator_t m_allocator{};
    bal_logger_t    m_logger{};
    bal_engine_t    m_engine{};
    bool            m_ready = false;
};

} // namespace NeXo2::Core
