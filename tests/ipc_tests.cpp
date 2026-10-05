// Tests de IPC: sm:, set:sys, dominios, TIPC y servicios que faltan.
//
// Usan tests/generated/ipc.nro (tests/programs/nro_ipc/main.c), un homebrew que
// construye los mensajes IPC a mano con el mismo formato que libnx.

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "system.hpp"
#include "hle/ipc.hpp"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;

namespace {

std::vector<u8> ReadNro(const char* name) {
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool Has(const std::string& text, const std::string& what) {
    const bool found = text.find(what) != std::string::npos;
    if (!found) std::printf("    No aparece en la salida: \"%s\"\n", what.c_str());
    return found;
}

} // namespace

TEST(Ipc_SmAndSetSys) {
    Core::System sys;
    CHECK(sys.LoadNro(ReadNro("ipc.nro"), "ipc.nro"));
    sys.GetCpu().Run(10'000'000);
    const std::string& out = sys.GetKernel().GetDebugOutput();

    CHECK(Has(out, "ConnectToNamedPort(sm:): rc=0x0"));
    CHECK(Has(out, "ConnectToNamedPort(xyz): rc=0xF201"));      // puerto inexistente
    CHECK(Has(out, "sm:RegisterClient: rc=0x0"));
    CHECK(Has(out, "QueryPointerBufferSize: rc=0x0 size=0x8000"));
    CHECK(Has(out, "GetServiceHandle(set:sys): rc=0x0"));
    CHECK(Has(out, "GetFirmwareVersion: rc=0x0 version=20.1.0 plataforma=NX"));
    CHECK(Has(out, "ConvertCurrentObjectToDomain: rc=0x0 objeto=1"));
    CHECK(Has(out, "Dominio -> GetFirmwareVersion2: rc=0x0 titulo=NeXo 2 HLE Firmware 20.1.0"));
    CHECK(Has(out, "TIPC GetServiceHandle(set:sys): rc=0x0"));
    CHECK(Has(out, "CloseHandle(set:sys TIPC): rc=0x0"));
    CHECK(Has(out, "SendSyncRequest(handle cerrado): rc=0xE401"));
    CHECK(Has(out, "GetServiceHandle(fsp-srv): rc=0x0"));
    CHECK(out.find("ERROR") == std::string::npos);
}

TEST(Ipc_UnimplementedServiceStopsCpuWithName) {
    Core::System sys;
    CHECK(sys.LoadNro(ReadNro("ipc.nro"), "ipc.nro"));
    sys.GetCpu().Run(10'000'000);
    CHECK(sys.GetCpu().IsHalted());
    CHECK(!sys.GetKernel().HasExited());
    CHECK(sys.GetCpu().GetHaltReason() == "Servicio 'fsp-srv': comando 1 no implementado");
}

TEST(Ipc_ServiceNameFromU64) {
    CHECK(HLE::ServiceNameFromU64(0x7379733A746573ULL) == "set:sys");
    CHECK(HLE::ServiceNameFromU64(0x3A6D73ULL) == "sm:");
    CHECK(HLE::ServiceNameFromU64(0).empty());
}

TEST(Ipc_HandlesAreClearedOnRestart) {
    Core::System sys;
    CHECK(sys.LoadNro(ReadNro("ipc.nro"), "ipc.nro"));
    sys.GetCpu().Run(10'000'000);
    CHECK(sys.GetKernel().Handles().Count() > 1);
    sys.Restart();
    CHECK_EQ(sys.GetKernel().Handles().Count(), 1); // solo el hilo principal
}
