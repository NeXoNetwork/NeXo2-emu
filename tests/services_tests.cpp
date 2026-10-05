// Tests de los servicios que libnx usa al arrancar: apm, appletOE, hid, time, fsp-srv.
//
// tests/generated/libnx_init.nro (tests/programs/nro_libnx_init/main.c) repite
// paso a paso el __appInit de libnx con los mismos comandos y formatos.

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "system.hpp"
#include "hle/services/time.hpp"

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

TEST(Services_LibnxStartupSequence) {
    Core::System sys;
    CHECK(sys.LoadNro(LoadTestNro("libnx_init.nro"), "libnx_init.nro"));
    sys.GetCpu().Run(20'000'000);
    const std::string& out = sys.GetKernel().GetDebugOutput();

    CHECK(sys.GetKernel().HasExited());
    CHECK(out.find("__appInit completo, fallos: 0") != std::string::npos);

    // Ninguna llamada ha fallado: imprimimos las que fallen para verlas en el test
    std::istringstream lines(out);
    std::string line;
    int ok_lines = 0;
    while (std::getline(lines, line)) {
        if (line.rfind("FALLO", 0) == 0) std::printf("    %s\n", line.c_str());
        CHECK(line.rfind("FALLO", 0) != 0);
        if (line.rfind("ok", 0) == 0) ++ok_lines;
    }
    CHECK_EQ(ok_lines, 45);

    CHECK(out.find("applet: aruid=0x101 foco=1 modo=0 running=1") != std::string::npos);
    CHECK(out.find("hid: memoria compartida tipo=0x6 tamano=0x40000") != std::string::npos);
    CHECK(out.find("zona=UTC") != std::string::npos);
    CHECK(out.find("fs: IFileSystem de la SD = objeto") != std::string::npos);
}

TEST(Services_SharedMemoryIsMapped) {
    Core::System sys;
    CHECK(sys.LoadNro(LoadTestNro("libnx_init.nro"), "libnx_init.nro"));
    sys.GetCpu().Run(20'000'000);
    const auto hid = sys.GetMemory().QueryRegion(0x180000000ULL);
    CHECK_EQ(static_cast<u32>(hid.state), static_cast<u32>(Core::MemoryState::Shared));
    CHECK_EQ(hid.size, 0x40000);
    // El reloj de la memoria compartida de time tiene la hora del PC (> 1 de enero de 2024)
    CHECK(sys.GetMemory().Read<u64>(0x180040040ULL) > 1704067200ULL);
}

TEST(Time_CalendarConversion) {
    HLE::CalendarTime cal{};
    HLE::CalendarAdditionalInfo info{};

    HLE::ToCalendarTime(0, cal, info);               // 1970-01-01 00:00:00, jueves
    CHECK_EQ(cal.year, 1970); CHECK_EQ(cal.month, 1); CHECK_EQ(cal.day, 1);
    CHECK_EQ(info.day_of_week, 4);

    HLE::ToCalendarTime(951782400 + 3723, cal, info); // 2000-02-29 01:02:03, martes (bisiesto)
    CHECK_EQ(cal.year, 2000); CHECK_EQ(cal.month, 2); CHECK_EQ(cal.day, 29);
    CHECK_EQ(cal.hour, 1); CHECK_EQ(cal.minute, 2); CHECK_EQ(cal.second, 3);
    CHECK_EQ(info.day_of_week, 2);
    CHECK_EQ(info.day_of_year, 59);

    HLE::ToCalendarTime(1791158400, cal, info);      // 2026-10-05 00:00:00, lunes
    CHECK_EQ(cal.year, 2026); CHECK_EQ(cal.month, 10); CHECK_EQ(cal.day, 5);
    CHECK_EQ(info.day_of_week, 1);
}

TEST(Kernel_WaitSynchronizationWithoutSignalStops) {
    // svcWaitSynchronization(handles=0, count=0, timeout=-1) sin nada que esperar
    Core::System sys;
    const u32 program[] = {
        0xD2800001u, // mov x1, #0
        0xD2800002u, // mov x2, #0
        0x92800003u, // mov x3, #-1
        0xD4000301u, // svc #0x18
    };
    sys.LoadRawProgram(program, 4, HLE::Layout::CODE_BASE);
    sys.GetCpu().Run(10);
    CHECK(sys.GetCpu().IsHalted());
    CHECK(sys.GetCpu().GetHaltReason().find("svcWaitSynchronization") != std::string::npos);
}
