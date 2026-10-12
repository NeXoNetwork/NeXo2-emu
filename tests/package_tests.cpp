// Tests de NSP / NCA / RomFS: se fabrica un NSP de prueba con claves inventadas (las de una
// consola de verdad no pueden ir en el repositorio) y se comprueba que NeXo lo abre igual que
// abriria uno real: cabecera con AES-XTS, area de claves, secciones con AES-CTR, PFS0 y RomFS.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "test_framework.hpp"
#include "system.hpp"
#include "crypto/aes.hpp"
#include "loader/package.hpp"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;
namespace fs = std::filesystem;

namespace {
using Bytes = std::vector<u8>;

Bytes ReadTest(const char* name) {
    std::ifstream f(std::string(NEXO2_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
template <typename T> void Put(Bytes& b, size_t off, T v) {
    if (b.size() < off + sizeof(T)) b.resize(off + sizeof(T));
    std::memcpy(&b[off], &v, sizeof(T));
}
void Align(Bytes& b, size_t a) { b.resize((b.size() + a - 1) / a * a, 0); }

Bytes Pfs0(const std::map<std::string, Bytes>& files) {
    Bytes strings;
    std::vector<u32> name_off;
    for (auto& [n, d] : files) { name_off.push_back(u32(strings.size())); strings.insert(strings.end(), n.begin(), n.end()); strings.push_back(0); }
    Align(strings, 0x20);
    Bytes out(0x10);
    std::memcpy(out.data(), "PFS0", 4);
    Put<u32>(out, 4, u32(files.size()));
    Put<u32>(out, 8, u32(strings.size()));
    u64 off = 0;
    size_t i = 0;
    Bytes data;
    for (auto& [n, d] : files) {
        const size_t e = 0x10 + i * 0x18;
        Put<u64>(out, e, off);
        Put<u64>(out, e + 8, d.size());
        Put<u32>(out, e + 16, name_off[i]);
        Put<u32>(out, e + 20, 0);
        data.insert(data.end(), d.begin(), d.end());
        off += d.size();
        ++i;
    }
    out.insert(out.end(), strings.begin(), strings.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// RomFS con todos los archivos en la raiz
Bytes Romfs(const std::map<std::string, Bytes>& files) {
    constexpr u32 NONE = 0xFFFFFFFF;
    Bytes dir_meta(0x18, 0), file_meta, data;
    Put<u32>(dir_meta, 0, 0); Put<u32>(dir_meta, 4, NONE); Put<u32>(dir_meta, 8, NONE);
    Put<u32>(dir_meta, 12, files.empty() ? NONE : 0); Put<u32>(dir_meta, 16, NONE); Put<u32>(dir_meta, 20, 0);
    size_t i = 0;
    for (auto& [n, d] : files) {
        const size_t e = file_meta.size();
        const size_t entry_size = (0x20 + n.size() + 3) / 4 * 4;
        file_meta.resize(e + entry_size, 0);
        Put<u32>(file_meta, e, 0);
        Put<u32>(file_meta, e + 4, i + 1 < files.size() ? u32(e + entry_size) : NONE);
        Put<u64>(file_meta, e + 8, data.size());
        Put<u64>(file_meta, e + 16, d.size());
        Put<u32>(file_meta, e + 24, NONE);
        Put<u32>(file_meta, e + 28, u32(n.size()));
        std::memcpy(&file_meta[e + 0x20], n.data(), n.size());
        data.insert(data.end(), d.begin(), d.end());
        Align(data, 0x10);
        ++i;
    }
    Bytes out(0x50, 0);
    const u64 dir_hash = 0x50, dir_meta_off = dir_hash + 4, file_hash = dir_meta_off + dir_meta.size();
    const u64 file_meta_off = file_hash + 4;
    u64 data_off = (file_meta_off + file_meta.size() + 0xF) & ~u64(0xF);
    Put<u64>(out, 0, 0x50);
    Put<u64>(out, 8, dir_hash); Put<u64>(out, 0x10, 4);
    Put<u64>(out, 0x18, dir_meta_off); Put<u64>(out, 0x20, dir_meta.size());
    Put<u64>(out, 0x28, file_hash); Put<u64>(out, 0x30, 4);
    Put<u64>(out, 0x38, file_meta_off); Put<u64>(out, 0x40, file_meta.size());
    Put<u64>(out, 0x48, data_off);
    Put<u32>(out, dir_hash, 0);
    out.insert(out.end(), dir_meta.begin(), dir_meta.end());
    Put<u32>(out, file_hash, 0);
    out.insert(out.end(), file_meta.begin(), file_meta.end());
    out.resize(data_off, 0);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

const Crypto::Key128 HEADER_KEY_A = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const Crypto::Key128 HEADER_KEY_B = {21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36};
const Crypto::Key128 KAEK = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
const Crypto::Key128 CTR_KEY = {0x55, 0x66, 0x77, 0x88, 0x99, 0x11, 0x22, 0x33, 0x44, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE};

// NCA de prueba: secciones (tipo 1 = PFS0, 0 = RomFS) cifradas con AES-CTR
Bytes Nca(u8 content_type, u64 program_id, const std::vector<std::pair<u8, Bytes>>& sections) {
    Bytes nca(0xC00, 0);
    std::memcpy(&nca[0x200], "NCA3", 4);
    nca[0x205] = content_type;
    Put<u64>(nca, 0x210, program_id);
    // Area de claves: la clave 2 (CTR), cifrada con key_area_key_application_00
    u8 area[0x40] = {};
    std::memcpy(area + 0x20, CTR_KEY.data(), 16);
    Crypto::Aes128 kaek(KAEK);
    for (int i = 0; i < 4; ++i) kaek.EncryptBlock(area + 16 * i, area + 16 * i);
    std::memcpy(&nca[0x300], area, 0x40);
    for (size_t s = 0; s < sections.size(); ++s) {
        Align(nca, 0x200);
        const u64 start = nca.size();
        Bytes data = sections[s].second;
        Align(data, 0x200);
        u8* fsh = &nca[0x400 + 0x200 * s];
        fsh[2] = sections[s].first;
        fsh[3] = sections[s].first == 1 ? 2 : 3;
        fsh[4] = 3;   // AES-CTR
        u64 len = sections[s].second.size();
        if (sections[s].first == 1) { u64 z = 0; std::memcpy(fsh + 0x40, &z, 8); std::memcpy(fsh + 0x48, &len, 8); }
        else { u64 z = 0; std::memcpy(fsh + 0x90, &z, 8); std::memcpy(fsh + 0x98, &len, 8); }
        for (int i = 0; i < 8; ++i) fsh[0x140 + i] = u8(0x10 * s + i + 1);
        u8 ctr_hi[8];
        for (int i = 0; i < 8; ++i) ctr_hi[i] = fsh[0x140 + 7 - i];
        Crypto::AesCtrCrypt(Crypto::Aes128(CTR_KEY), ctr_hi, start, data.data(), data.size());
        nca.insert(nca.end(), data.begin(), data.end());
        Put<u32>(nca, 0x240 + 0x10 * s, u32(start / 0x200));
        Put<u32>(nca, 0x244 + 0x10 * s, u32(nca.size() / 0x200));
    }
    Put<u64>(nca, 0x208, nca.size());
    Crypto::AesXtsEncrypt(Crypto::Aes128(HEADER_KEY_A), Crypto::Aes128(HEADER_KEY_B), nca.data(), 0xC00, 0, 0x200);
    return nca;
}

// NSO sin comprimir a partir de un NRO
Bytes NroToNso(const Bytes& nro) {
    u32 h[0x1C];
    std::memcpy(h, nro.data() + 0x10, sizeof(h));
    const u32 to = h[4], ts = h[5], ro = h[6], rs = h[7], dof = h[8], ds = h[9], bss = h[10];
    Bytes n(0x100, 0);
    std::memcpy(n.data(), "NSO0", 4);
    Put<u32>(n, 0x10, 0x100); Put<u32>(n, 0x14, to); Put<u32>(n, 0x18, ts);
    Put<u32>(n, 0x20, 0x100 + ts); Put<u32>(n, 0x24, ro); Put<u32>(n, 0x28, rs);
    Put<u32>(n, 0x30, 0x100 + ts + rs); Put<u32>(n, 0x34, dof); Put<u32>(n, 0x38, ds);
    Put<u32>(n, 0x3C, bss);
    Put<u32>(n, 0x60, ts); Put<u32>(n, 0x64, rs); Put<u32>(n, 0x68, ds);
    n.insert(n.end(), nro.begin() + to, nro.begin() + to + ts);
    n.insert(n.end(), nro.begin() + ro, nro.begin() + ro + rs);
    n.insert(n.end(), nro.begin() + dof, nro.begin() + dof + ds);
    return n;
}

std::string Hex(const u8* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
void WriteFile(const fs::path& p, const Bytes& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}
} // namespace

TEST(Crypto_AesKnownAnswer) {
    // FIPS-197, apendice C.1
    const Crypto::Key128 key = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const u8 pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const u8 ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    Crypto::Aes128 aes(key);
    u8 out[16], back[16];
    aes.EncryptBlock(pt, out);
    CHECK(std::memcmp(out, ct, 16) == 0);
    aes.DecryptBlock(ct, back);
    CHECK(std::memcmp(back, pt, 16) == 0);
    // XTS: cifrar y descifrar vuelve a lo mismo; CTR desde la mitad = CTR de todo
    Bytes data(0x400);
    for (size_t i = 0; i < data.size(); ++i) data[i] = u8(i * 7);
    Bytes x = data;
    Crypto::Aes128 k2(Crypto::Key128{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9});
    Crypto::AesXtsEncrypt(aes, k2, x.data(), x.size(), 3, 0x200);
    CHECK(x != data);
    Crypto::AesXtsDecrypt(aes, k2, x.data(), x.size(), 3, 0x200);
    CHECK(x == data);
    const u8 ctr[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    Bytes whole = data, part(data.begin() + 0x123, data.begin() + 0x200);
    Crypto::AesCtrCrypt(aes, ctr, 0, whole.data(), whole.size());
    Crypto::AesCtrCrypt(aes, ctr, 0x123, part.data(), part.size());
    CHECK(std::memcmp(part.data(), whole.data() + 0x123, part.size()) == 0);
}

TEST(Package_NspLoadsAndRuns) {
    const fs::path dir = fs::temp_directory_path() / "nexo2_test_nsp";
    fs::remove_all(dir);
    fs::create_directories(dir / "keys");
    fs::create_directories(dir / "sdmc");

    // NSP: NCA de programa (ExeFS + RomFS) y NCA de control (control.nacp)
    Bytes npdm(0xC0, 0);
    Put<u32>(npdm, 0, 0x4154454D); npdm[0x0C] = 1 | (3 << 1); npdm[0x0E] = 44; Put<u32>(npdm, 0x1C, 0x100000);
    Put<u32>(npdm, 0x70, 0x80); Put<u32>(npdm, 0x74, 0x40); Put<u32>(npdm, 0x80, 0x30494341); Put<u64>(npdm, 0x90, 0x0100123400000000ULL);
    const Bytes exefs = Pfs0({{"main", NroToNso(ReadTest("hello.nro"))}, {"main.npdm", npdm}});
    const std::string text = "datos del juego";
    const Bytes romfs = Romfs({{"a.bin", Bytes(100, 7)}, {"hello.txt", Bytes(text.begin(), text.end())}});
    Bytes nacp(0x4000, 0);
    std::memcpy(nacp.data(), "Juego NSP de prueba", 19);
    // Como en los juegos de verdad: ExeFS (0), RomFS (1) y el logo, otro PFS0 (2)
    const Bytes logo = Pfs0({{"NintendoLogo.png", Bytes(64, 1)}, {"StartupMovie.gif", Bytes(64, 2)}});
    const Bytes prog = Nca(0, 0x0100123400000000ULL, {{1, exefs}, {0, romfs}, {1, logo}});
    const Bytes ctrl = Nca(2, 0x0100123400000000ULL, {{0, Romfs({{"control.nacp", nacp}})}});
    WriteFile(dir / "juego.nsp", Pfs0({{"0123.nca", prog}, {"4567.nca", ctrl}}));

    Core::System sys;
    sys.GetKernel().SetSdmcRoot(dir / "sdmc");
    // Sin claves: mensaje claro
    CHECK(!sys.LoadGame((dir / "juego.nsp").string()));
    CHECK(sys.GetLastError().find("prod.keys") != std::string::npos);

    std::ofstream keys(dir / "keys" / "prod.keys");
    keys << "header_key = " << Hex(HEADER_KEY_A.data(), 16) << Hex(HEADER_KEY_B.data(), 16) << "\n";
    keys << "key_area_key_application_00 = " << Hex(KAEK.data(), 16) << "\n";
    keys.close();

    CHECK(Core::System::IsGameFolder((dir / "juego.nsp").string()));
    CHECK(sys.LoadGame((dir / "juego.nsp").string()));
    if (!sys.GetLastError().empty()) std::printf("    %s\n", sys.GetLastError().c_str());
    CHECK(sys.GetProgramName() == "Juego NSP de prueba");
    auto& k = sys.GetKernel();
    CHECK_EQ(k.ProgramId(), 0x0100123400000000ULL);
    auto f = Loader::OpenRomfsFile(k.GetRomfs(), "/hello.txt");
    CHECK(f != nullptr);
    if (f) { const Bytes got = f->ReadAll(); CHECK(std::string(got.begin(), got.end()) == text); }
    u64 n = 0;
    while (n < 50'000'000 && !sys.GetCpu().IsHalted()) n += sys.Run(1'000'000);
    CHECK(k.GetDebugOutput().find("fibonacci(50) = 12586269025") != std::string::npos);

    // La biblioteca: nombre del juego sin cargar el programa
    Loader::KeySet ks;
    std::string where;
    CHECK(Loader::LoadKeys(sys.KeyDirs(), ks, where));
    auto info = Loader::OpenGamePackage(dir / "juego.nsp", ks, false);
    CHECK(info.ok && info.nacp.size() == 0x4000);
    fs::remove_all(dir);
}
