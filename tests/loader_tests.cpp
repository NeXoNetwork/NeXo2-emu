// Tests del cargador de NRO y del kernel HLE.
//
// Usan tests/generated/hello.nro (generado con tools/make_nro.py a partir de
// tests/programs/nro_hello/): un homebrew minimo que llama a varias SVC y
// escribe resultados con svcOutputDebugString.

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "system.hpp"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;
using Core::MemoryPermission;
using Core::MemoryState;

namespace {

std::vector<u8> ReadTestFile(const char* name) {
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const std::string& what) {
    const bool found = text.find(what) != std::string::npos;
    if (!found) std::printf("    No aparece en la salida: \"%s\"\n", what.c_str());
    return found;
}

} // namespace

TEST(Nro_LoadsAndMapsSegments) {
    const std::vector<u8> nro = ReadTestFile("hello.nro");
    CHECK(!nro.empty());

    Core::System sys;
    CHECK(sys.LoadNro(nro, "hello.nro"));
    const Loader::NroInfo* info = sys.GetNroInfo();
    CHECK(info != nullptr);
    if (!info) return;

    CHECK_EQ(info->base, HLE::Layout::CODE_BASE);
    CHECK_EQ(info->entry, HLE::Layout::CODE_BASE);
    CHECK(info->has_assets);
    CHECK(info->title == "NeXo Hello");
    CHECK(info->author == "NeXo 2 tests");

    // .text = codigo ejecutable, .data = lectura/escritura
    const auto text = sys.GetMemory().QueryRegion(info->base);
    CHECK_EQ(static_cast<u32>(text.state), static_cast<u32>(MemoryState::Code));
    CHECK_EQ(static_cast<u32>(text.perm), static_cast<u32>(MemoryPermission::ReadExecute));
    const auto data = sys.GetMemory().QueryRegion(info->base + info->header.data_offset);
    CHECK_EQ(static_cast<u32>(data.state), static_cast<u32>(MemoryState::CodeData));
    CHECK_EQ(static_cast<u32>(data.perm), static_cast<u32>(MemoryPermission::ReadWrite));

    // Registros de entrada del Homebrew ABI
    const auto& st = sys.GetCpu().GetState();
    CHECK_EQ(st.pc, info->entry);
    CHECK_EQ(st.x[1], ~0ULL);
    CHECK_EQ(st.sp, HLE::Layout::STACK_REGION_BASE + HLE::Layout::MAIN_STACK_SIZE);
    CHECK_EQ(st.tpidrro_el0, HLE::Layout::TLS_PAGE);
}

TEST(Nro_HelloRunsToExit) {
    Core::System sys;
    CHECK(sys.LoadNro(ReadTestFile("hello.nro"), "hello.nro"));
    sys.GetCpu().Run(10'000'000);

    CHECK(sys.GetKernel().HasExited());
    const std::string& out = sys.GetKernel().GetDebugOutput();
    CHECK(Contains(out, "Hola desde un NRO en NeXo 2!"));
    CHECK(Contains(out, "Loader: X1=0xFFFFFFFFFFFFFFFF, entradas de config=4"));
    CHECK(Contains(out, "argv: hello.nro"));
    CHECK(Contains(out, "Contador (.data): 42"));
    CHECK(Contains(out, "svcSetHeapSize: rc=0x0, heap en 0x80000000"));
    CHECK(Contains(out, "Suma en el heap: 499500"));
    CHECK(Contains(out, "QueryMemory(main): base=0x8000000 tipo=0x3 permisos=0x5"));
    CHECK(Contains(out, "GetInfo(HeapRegionAddress): 0x80000000"));
    CHECK(Contains(out, "fibonacci(50) = 12586269025"));
    CHECK(Contains(out, "Adios!"));
    CHECK_EQ(sys.GetKernel().GetHeapSize(), 0x200000);
}

TEST(Nro_RestartRunsAgain) {
    Core::System sys;
    CHECK(sys.LoadNro(ReadTestFile("hello.nro"), "hello.nro"));
    sys.GetCpu().Run(10'000'000);
    CHECK(sys.GetKernel().HasExited());
    sys.Restart();
    CHECK(!sys.GetKernel().HasExited());
    CHECK(sys.GetKernel().GetDebugOutput().empty());
    sys.GetCpu().Run(10'000'000);
    CHECK(sys.GetKernel().HasExited());
    // .data vuelve a su valor original: el contador sigue saliendo 42, no 43
    CHECK(Contains(sys.GetKernel().GetDebugOutput(), "Contador (.data): 42"));
}

TEST(Nro_RejectsInvalidFiles) {
    Core::System sys;
    std::vector<u8> nro = ReadTestFile("hello.nro");

    CHECK(!sys.LoadNro(std::vector<u8>(16, 0), "corto.nro"));
    CHECK(sys.GetLastError().find("pequeno") != std::string::npos);

    std::vector<u8> bad_magic = nro;
    bad_magic[0x10] = 'X';
    CHECK(!sys.LoadNro(bad_magic, "magic.nro"));
    CHECK(sys.GetLastError().find("NRO0") != std::string::npos);

    std::vector<u8> truncated(nro.begin(), nro.begin() + 0x200);
    CHECK(!sys.LoadNro(truncated, "cortado.nro"));
    CHECK(sys.GetLastError().find("cortado") != std::string::npos);
}

TEST(Kernel_UnknownSvcStopsCpu) {
    Core::System sys;
    const u32 program[] = { 0xD4000441u /* svc #0x22 */ };
    sys.LoadRawProgram(program, 1, HLE::Layout::CODE_BASE);
    sys.GetCpu().Run(10);
    CHECK(sys.GetCpu().IsHalted());
    CHECK(sys.GetCpu().GetHaltReason().find("SendSyncRequestWithUserBuffer") != std::string::npos);
    CHECK(std::string(HLE::Kernel::SvcName(0x29)) == "GetInfo");
    CHECK(std::string(HLE::Kernel::SvcName(0x6F)) == "GetSystemInfo");
    CHECK(std::string(HLE::Kernel::SvcName(0x7F)) == "CallSecureMonitor");
}

TEST(Memory_RegionsSplitAndQuery) {
    Core::Memory mem;
    mem.MapRegion(0x10000, 0x10000, MemoryState::Normal, MemoryPermission::ReadWrite, "a");
    // Pisar el centro parte la zona en tres
    mem.MapRegion(0x14000, 0x4000, MemoryState::Code, MemoryPermission::ReadExecute, "b");
    CHECK_EQ(mem.Regions().size(), 3);
    CHECK_EQ(mem.QueryRegion(0x12000).size, 0x4000);
    CHECK_EQ(static_cast<u32>(mem.QueryRegion(0x15000).state), static_cast<u32>(MemoryState::Code));
    CHECK_EQ(mem.QueryRegion(0x18000).base, 0x18000);
    // Hueco libre antes y despues
    const auto before = mem.QueryRegion(0x100);
    CHECK_EQ(before.base, 0);
    CHECK_EQ(before.size, 0x10000);
    CHECK_EQ(static_cast<u32>(before.state), static_cast<u32>(MemoryState::Free));
    CHECK_EQ(mem.QueryRegion(0x20000).base, 0x20000);
    // Quitar una parte
    mem.UnmapRegion(0x10000, 0x8000);
    CHECK_EQ(mem.Regions().size(), 1);
    CHECK_EQ(mem.QueryRegion(0x18000).base, 0x18000);
}
