// Tests del modo multinucleo (Kernel::StartCores): cada nucleo emulado en su propio hilo
// del PC. Los mismos programas que en el modo de un hilo, con el reloj real.
//
// Lo que NO es determinista aqui es el orden entre nucleos; los programas comprueban
// su propio resultado (sumas con mutex, avisos con variables de condicion, IPC...).
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"
#include "system.hpp"
#include "common/logger.hpp"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;

namespace {
std::vector<u8> ReadTestNro(const char* name) {
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Arranca los nucleos y espera a que el programa pare (o a 'done', o al plazo)
template <typename Done>
bool RunCores(Core::System& sys, Done done, double seconds = 20.0) {
    auto& k = sys.GetKernel();
    k.SetMulticore(true);
    k.StartCores();
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    bool finished = false;
    while (std::chrono::steady_clock::now() < end) {
        if (k.CoresHalted()) { finished = true; break; }
        {
            auto lock = k.Lock();
            if (done(k)) { finished = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    k.StopCores();
    return finished;
}
} // namespace

TEST(Multicore_FourThreadsOnFourHostThreads) {
    // Varias veces: un fallo de sincronizacion entre hilos del PC no sale siempre
    for (int round = 0; round < 4; ++round) {
        Core::System sys;
        CHECK(sys.LoadNro(ReadTestNro("threads.nro"), "threads.nro"));
        Common::Logger::SetMuted(true);
        const bool finished = RunCores(sys, [](HLE::Kernel&) { return false; });
        Common::Logger::SetMuted(false);
        CHECK(finished);
        auto& k = sys.GetKernel();
        const std::string& out = k.GetDebugOutput();
        CHECK(out.find("hilos completo, fallos: 0") != std::string::npos);
        CHECK(out.find("FALLO") == std::string::npos);
        if (out.find("fallos: 0") == std::string::npos) std::printf("%s\n", out.c_str());
        CHECK(k.HasExited());
        CHECK(!k.CoresRunning());
        // Los nucleos 1, 2 y 3 ejecutaron codigo en su propia CPU
        for (s32 c = 1; c <= 3; ++c) CHECK(k.CoreCpu(c) && k.CoreCpu(c)->GetInstructionCount() > 0);
        CHECK(sys.GetCpu().IsHalted());
    }
}

TEST(Multicore_LibnxServicesAndIpc) {
    // __appInit de libnx entero (servicios, eventos, memoria compartida) y el programa de IPC
    {
        Core::System sys;
        CHECK(sys.LoadNro(ReadTestNro("libnx_init.nro"), "libnx_init.nro"));
        Common::Logger::SetMuted(true);
        RunCores(sys, [](HLE::Kernel& k) {
            return k.GetDebugOutput().find("__appInit completo") != std::string::npos;
        });
        Common::Logger::SetMuted(false);
        const std::string& out = sys.GetKernel().GetDebugOutput();
        CHECK(out.find("__appInit completo, fallos: 0") != std::string::npos);
    }
    {
        Core::System sys;
        CHECK(sys.LoadNro(ReadTestNro("ipc.nro"), "ipc.nro"));
        Common::Logger::SetMuted(true);
        RunCores(sys, [](HLE::Kernel&) { return false; }, 10.0);
        Common::Logger::SetMuted(false);
        const std::string& out = sys.GetKernel().GetDebugOutput();
        CHECK(out.find("GetFirmwareVersion: rc=0x0 version=20.1.0 plataforma=NX") != std::string::npos);
        CHECK(out.find("TIPC GetServiceHandle(set:sys): rc=0x0") != std::string::npos);
        CHECK(sys.GetCpu().IsHalted());   // el ultimo comando no existe: para la CPU (en el nucleo que sea)
        CHECK(out.find("ERROR") == std::string::npos);
        CHECK(out.find("FALLO") == std::string::npos);
    }
}

TEST(Multicore_GpuThreadAndTriangle) {
    // GPU en su hilo + nucleos en sus hilos: el programa dibuja, espera al fence y mira los pixeles
    Core::System sys;
    CHECK(sys.LoadNro(ReadTestNro("gpu.nro"), "gpu.nro"));
    Common::Logger::SetMuted(true);
    RunCores(sys, [](HLE::Kernel& k) { return k.GetDebugOutput().find("gpu completo") != std::string::npos; });
    Common::Logger::SetMuted(false);
    const std::string& out = sys.GetKernel().GetDebugOutput();
    CHECK(out.find("gpu completo, fallos: 0") != std::string::npos);
    if (out.find("fallos: 0") == std::string::npos) std::printf("%s\n", out.c_str());
}

TEST(Multicore_StopAndResumeKeepsState) {
    // Parar y seguir varias veces a mitad del programa: los registros vuelven a cada hilo
    Core::System sys;
    CHECK(sys.LoadNro(ReadTestNro("threads.nro"), "threads.nro"));
    auto& k = sys.GetKernel();
    k.SetMulticore(true);
    Common::Logger::SetMuted(true);
    bool done = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!done && std::chrono::steady_clock::now() < end) {
        k.StartCores();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        k.StopCores();
        done = k.HasExited() || sys.GetCpu().IsHalted();
    }
    Common::Logger::SetMuted(false);
    CHECK(done);
    CHECK(k.GetDebugOutput().find("hilos completo, fallos: 0") != std::string::npos);
    // Volver al modo de un hilo
    k.SetMulticore(false);
    CHECK(!k.IsMulticore());
}
