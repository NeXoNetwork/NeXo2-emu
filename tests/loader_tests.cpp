// Tests del cargador de NRO y del kernel HLE.
//
// Usan tests/generated/hello.nro (generado con tools/make_nro.py a partir de
// tests/programs/nro_hello/): un homebrew minimo que llama a varias SVC y
// escribe resultados con svcOutputDebugString.

#include <cstring>
#include <filesystem>
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
    sys.Run(10'000'000);

    CHECK(sys.GetKernel().HasExited());
    const std::string& out = sys.GetKernel().GetDebugOutput();
    CHECK(Contains(out, "Hola desde un NRO en NeXo 2!"));
    CHECK(Contains(out, "Loader: X1=0xFFFFFFFFFFFFFFFF, entradas de config=7"));   // + NextLoadPath y ProcessHandle
    CHECK(Contains(out, "argv: sdmc:/switch/hello.nro"));
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
    sys.Run(10'000'000);
    CHECK(sys.GetKernel().HasExited());
    sys.Restart();
    CHECK(!sys.GetKernel().HasExited());
    CHECK(sys.GetKernel().GetDebugOutput().empty());
    sys.Run(10'000'000);
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
    const u32 program[] = { 0xD40005C1u /* svc #0x2E */ };
    sys.LoadRawProgram(program, 1, HLE::Layout::CODE_BASE);
    sys.Run(10);
    CHECK(sys.GetCpu().IsHalted());
    CHECK(sys.GetCpu().GetHaltReason().find("GetDebugFutureThreadInfo") != std::string::npos);
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

// ---------------------------------------------------------------------------
// Juegos extraidos: NSO (con LZ4) + main.npdm + romfs.bin
// ---------------------------------------------------------------------------

TEST(Nso_Lz4Block) {
    // "abcd" + copia de 8 bytes 4 atras + "e" = "abcdabcdabcde"
    const u8 block[] = {0x44, 'a', 'b', 'c', 'd', 0x04, 0x00, 0x10, 'e'};
    u8 out[13] = {};
    CHECK(Loader::Lz4DecompressBlock(block, sizeof(block), out, sizeof(out)));
    CHECK(std::string(reinterpret_cast<char*>(out), 13) == "abcdabcdabcde");
    // Copia mas larga que su distancia (se solapa) y longitudes de mas de 15
    const u8 rle[] = {0x1F, 'x', 0x01, 0x00, 0x05, 0x10, 'y'};   // x + 24 x (15+5+4) + y
    u8 out2[26] = {};
    CHECK(Loader::Lz4DecompressBlock(rle, sizeof(rle), out2, sizeof(out2)));
    CHECK(std::string(reinterpret_cast<char*>(out2), 26) == std::string(25, 'x') + "y");
    // Datos rotos: offset que apunta antes del principio
    const u8 bad[] = {0x10, 'a', 0x05, 0x00, 0x10, 'b'};
    u8 out3[8] = {};
    CHECK(!Loader::Lz4DecompressBlock(bad, sizeof(bad), out3, sizeof(out3)));
}

namespace {
// LZ4 "sin comprimir": un solo bloque de literales (valido para cualquier descompresor)
std::vector<u8> Lz4Literals(const u8* data, size_t n) {
    std::vector<u8> out;
    if (n < 15) { out.push_back(u8(n << 4)); }
    else { out.push_back(0xF0); size_t rest = n - 15; while (rest >= 255) { out.push_back(255); rest -= 255; } out.push_back(u8(rest)); }
    out.insert(out.end(), data, data + n);
    return out;
}

// Convierte hello.nro en un NSO (.text comprimido, el resto tal cual)
std::vector<u8> NroToNso(const std::vector<u8>& nro) {
    Loader::NroHeader h{};
    std::memcpy(&h, nro.data() + 0x10, sizeof(h));
    Loader::NsoHeader n{};
    n.magic = 0x304F534E;
    n.flags = 1;                                      // .text con LZ4
    const auto text = Lz4Literals(nro.data() + h.text_offset, h.text_size);
    n.text = {0x100, h.text_offset, h.text_size};
    n.text_file_size = u32(text.size());
    n.ro = {u32(0x100 + text.size()), h.ro_offset, h.ro_size};
    n.ro_file_size = h.ro_size;
    n.data = {n.ro.file_offset + h.ro_size, h.data_offset, h.data_size};
    n.data_file_size = h.data_size;
    n.bss_size = h.bss_size;
    std::vector<u8> out(sizeof(n));
    std::memcpy(out.data(), &n, sizeof(n));
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), nro.begin() + h.ro_offset, nro.begin() + h.ro_offset + h.ro_size);
    out.insert(out.end(), nro.begin() + h.data_offset, nro.begin() + h.data_offset + h.data_size);
    return out;
}

std::vector<u8> MakeNpdm(u8 priority, u32 stack, u64 program_id) {
    std::vector<u8> f(0x80 + 0x40, 0);
    auto put32 = [&](size_t o, u32 v) { std::memcpy(&f[o], &v, 4); };
    put32(0, 0x4154454D);                             // META
    f[0x0C] = 1 | (3 << 1);                           // 64 bits, espacio de 39 bits
    f[0x0E] = priority;
    f[0x0F] = 0;
    put32(0x1C, stack);
    std::memcpy(&f[0x20], "NeXoTest", 8);
    put32(0x70, 0x80); put32(0x74, 0x40);             // ACI0
    put32(0x80, 0x30494341);
    std::memcpy(&f[0x90], &program_id, 8);
    return f;
}

void WriteFile(const std::filesystem::path& p, const std::vector<u8>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}
} // namespace

TEST(Game_LoadsExtractedExefs) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "nexo2_test_game";
    std::error_code ec0;
    fs::remove_all(dir, ec0);
    fs::create_directories(dir / "exefs");
    WriteFile(dir / "exefs" / "main", NroToNso(ReadTestFile("hello.nro")));
    WriteFile(dir / "exefs" / "main.npdm", MakeNpdm(49, 0x40000, 0x0100ABCD00000000ULL));
    WriteFile(dir / "romfs.bin", std::vector<u8>(0x1234, 0xAB));

    CHECK(Core::System::IsGameFolder(dir.string()));
    CHECK(!Core::System::IsGameFolder(fs::temp_directory_path().string()));
    {   // (el sistema tiene romfs.bin abierto: en Windows no se puede borrar hasta cerrarlo)
    Core::System sys;
    CHECK(sys.LoadGame(dir.string()));
    auto& k = sys.GetKernel();
    CHECK(k.IsApplication());
    CHECK_EQ(k.ProgramId(), 0x0100ABCD00000000ULL);
    CHECK(k.GetRomfs() && k.GetRomfs()->Size() == 0x1234);
    CHECK(k.GetSaveRoot().filename() == "0100ABCD00000000");
    const auto& st = sys.GetCpu().GetState();
    CHECK_EQ(st.pc, HLE::Layout::CODE_BASE);
    CHECK_EQ(st.x[0], 0ULL);
    CHECK_EQ(st.sp, HLE::Layout::STACK_REGION_BASE + 0x40000);
    CHECK_EQ(k.Threads().front()->priority, 49);
    const auto text = sys.GetMemory().QueryRegion(HLE::Layout::CODE_BASE);
    CHECK_EQ(static_cast<u32>(text.perm), static_cast<u32>(MemoryPermission::ReadExecute));

    u64 n = 0;
    while (n < 50'000'000 && !sys.GetCpu().IsHalted()) n += sys.Run(1'000'000);
    const std::string& out = k.GetDebugOutput();
    CHECK(Contains(out, "Hola desde un NRO en NeXo 2!"));
    CHECK(Contains(out, "Loader: X1=0x10001"));                 // X1 = handle del hilo principal
    CHECK(Contains(out, "fibonacci(50) = 12586269025"));
    CHECK(k.HasExited());

    sys.Restart();                                             // vuelve a cargar el juego
    CHECK(sys.GetKernel().IsApplication());
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
