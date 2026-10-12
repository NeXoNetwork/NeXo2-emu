// Tests de los hilos y el planificador (kernel_threads.cpp).
//
// tests/generated/threads.nro (tests/programs/nro_threads/main.c) crea 4 hilos en
// 4 nucleos que suman en un contador compartido protegido por un mutex estilo libnx,
// se avisan con una variable de condicion y terminan. Comprueba ademas dormir,
// plazos vencidos y esperar a que un hilo termine.
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "system.hpp"
#include "common/logger.hpp"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;

namespace {
std::vector<u8> LoadTestNro(const char* name) {
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
} // namespace

TEST(Threads_FourThreadsMutexCondvar) {
    Core::System sys;
    CHECK(sys.LoadNro(LoadTestNro("threads.nro"), "threads.nro"));
    Common::Logger::SetMuted(true);
    sys.Run(50'000'000);
    Common::Logger::SetMuted(false);

    const std::string& out = sys.GetKernel().GetDebugOutput();
    CHECK(out.find("hilos completo, fallos: 0") != std::string::npos);
    CHECK(out.find("FALLO") == std::string::npos);
    CHECK(sys.GetKernel().HasExited());

    // Que lo dificil se haya ejercitado de verdad
    const auto& st = sys.GetKernel().Stats();
    CHECK(st.mutex_waits > 10);       // hilos durmiendo esperando el mutex
    CHECK(st.condvar_waits >= 1);
    CHECK(st.context_switches > 10);
    CHECK(st.idle_ticks > 0);         // svcSleepThread con todos dormidos: el reloj salto

    // 1 principal + 4 trabajadores + 1 que nunca arranco: al salir del proceso, todos terminados
    const auto& threads = sys.GetKernel().Threads();
    CHECK_EQ(threads.size(), 6);
    int terminated = 0;
    for (const auto& t : threads)
        if (t->state == HLE::KThread::State::Terminated) ++terminated;
    CHECK_EQ(terminated, 6);
}

TEST(Threads_SameResultWithoutDecodeCache) {
    // El mismo programa con hilos, con y sin cache: tiene que dar exactamente lo mismo
    Core::System a, b;
    a.GetCpu().SetDecodeCacheEnabled(false);
    CHECK(a.LoadNro(LoadTestNro("threads.nro"), "threads.nro"));
    CHECK(b.LoadNro(LoadTestNro("threads.nro"), "threads.nro"));
    Common::Logger::SetMuted(true);
    const u64 na = a.Run(50'000'000);
    const u64 nb = b.Run(50'000'000);
    Common::Logger::SetMuted(false);
    CHECK_EQ(na, nb);
    CHECK_EQ(a.GetCpu().GetInstructionCount(), b.GetCpu().GetInstructionCount());
    CHECK(a.GetKernel().GetDebugOutput() == b.GetKernel().GetDebugOutput());
    CHECK_EQ(a.GetKernel().Stats().context_switches, b.GetKernel().Stats().context_switches);
}

TEST(Threads_DeadlockIsReported) {
    // El hilo principal espera para siempre a... si mismo (nunca termina mientras espera).
    // Nadie puede despertarlo: el planificador tiene que pararse con un mensaje claro.
    Core::System sys;
    CHECK(sys.LoadNro(LoadTestNro("hello.nro"), "hello.nro"));
    const u32 program[] = {
        0x52800029u,  // mov  w9, #1
        0x72A00029u,  // movk w9, #1, lsl #16      -> 0x00010001 (handle del hilo principal)
        0xB81F0FE9u,  // str  w9, [sp, #-16]!
        0x910003E1u,  // mov  x1, sp               (lista de handles)
        0xD2800022u,  // mov  x2, #1               (1 handle)
        0x92800003u,  // mov  x3, #-1              (sin plazo)
        0xD4000301u,  // svc  #0x18                (WaitSynchronization)
        0xD4200000u,  // brk  #0                   (no deberia llegar)
    };
    const u64 entry = sys.GetCpu().GetState().pc;
    for (u32 i = 0; i < 8; ++i) sys.GetMemory().Write<u32>(entry + i * 4, program[i]);
    Common::Logger::SetMuted(true);
    sys.Run(1'000'000);
    Common::Logger::SetMuted(false);
    CHECK(sys.GetCpu().IsHalted());
    CHECK(sys.GetCpu().GetHaltReason().find("Bloqueo") != std::string::npos);
}

TEST(Threads_AddressArbiter) {
    // svcWaitForAddress / svcSignalToAddress (los usan nn::os en los juegos) con un hilo:
    // condiciones que no se cumplen, un plazo que vence y los "signal ... if equal"
    Core::System sys;
    CHECK(sys.LoadNro(LoadTestNro("hello.nro"), "hello.nro"));
    const u32 program[] = {
        0xd10083ffu, 0x910003f3u, 0x528000a9u, 0xb9000269u,               // x19 = sp - 32; [x19] = 5
        0xaa1303e0u, 0x52800041u, 0xd28000e2u, 0x92800003u, 0xd4000681u,  // WaitIfEqual(7)
        0xb9000660u,
        0xaa1303e0u, 0x52800021u, 0xd2800142u, 0xd2807d03u, 0xd4000681u,  // DecrementAndWaitIfLess(10), 1000 ns
        0xb9000a60u,
        0xaa1303e0u, 0x52800021u, 0x52800082u, 0x52800023u, 0xd40006a1u,  // SignalAndIncrementIfEqual(4)
        0xb9000e60u,
        0xaa1303e0u, 0x52800021u, 0x52800122u, 0x52800023u, 0xd40006a1u,  // SignalAndIncrementIfEqual(9)
        0xb9001260u,
        0xd4200000u,                                                       // brk #0
    };
    const u64 entry = sys.GetCpu().GetState().pc;
    const u64 data = sys.GetCpu().GetState().sp - 32;
    for (u32 i = 0; i < std::size(program); ++i) sys.GetMemory().Write<u32>(entry + i * 4, program[i]);
    Common::Logger::SetMuted(true);
    sys.Run(1'000'000);
    Common::Logger::SetMuted(false);
    auto& mem = sys.GetMemory();
    CHECK_EQ(mem.Read<u32>(data + 4), 0xFA01u);    // InvalidState: el valor no era 7
    CHECK_EQ(mem.Read<u32>(data + 8), 0xEA01u);    // TimedOut tras decrementar
    CHECK_EQ(mem.Read<u32>(data + 12), 0u);        // Success
    CHECK_EQ(mem.Read<u32>(data + 16), 0xFA01u);   // InvalidState: el valor no era 9
    CHECK_EQ(mem.Read<u32>(data), 5u);             // 5 -> 4 (decremento) -> 5 (incremento)
}
