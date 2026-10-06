#pragma once
// JIT de NeXo 2: traduce bloques de codigo ARM64 a codigo x86-64 con dynarmic
// (externals/dynarmic) y los ejecuta directamente en el procesador del PC.
//
// No es una CPU aparte: es otra forma de hacer Interpreter::Run(). El estado de
// los registros sigue estando en Interpreter::GetState() entre llamadas, asi que el
// kernel HLE, los hilos y la interfaz no notan la diferencia. Ver
// docs/07-nexo-internals/jit.md.
//
// Las cabeceras de dynarmic solo se incluyen en jit_dynarmic.cpp: el resto del
// proyecto no depende de ellas.
#include <memory>
#include "common/types.hpp"

namespace NeXo2::Core {

class Interpreter;
class Memory;
class JitImpl;   // jit_dynarmic.cpp

class JitBackend {
public:
    JitBackend(Interpreter& cpu, Memory& memory);
    ~JitBackend();
    JitBackend(const JitBackend&) = delete;
    JitBackend& operator=(const JitBackend&) = delete;

    // Ejecuta hasta 'max_steps' instrucciones (puede pasarse un poco: el JIT
    // cuenta por bloques). Devuelve cuantas ejecuto.
    u64 Run(u64 max_steps);

    // Que el JIT termine cuanto antes (desde un SVC, Halt(), RequestStop()...)
    void RequestHalt();
    void ClearExclusive();
    // Tirar todo el codigo traducido (otro programa, o se cambio de modo)
    void ClearCache();

    // Estadisticas para la ventana de diagnostico
    struct Stats {
        u64 runs = 0;              // llamadas a Run()
        u64 svc_calls = 0;
        u64 fallbacks = 0;         // instrucciones que hizo el interprete (el JIT no las sabe)
        u64 invalidations = 0;     // paginas de codigo reescritas por el programa
    };
    const Stats& GetStats() const;

    // true si NeXo se compilo con el JIT (externals/dynarmic presente)
    static bool Available();

private:
    std::unique_ptr<JitImpl> m_impl;
};

} // namespace NeXo2::Core
