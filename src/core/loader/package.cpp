#include "package.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <mutex>
#include <sstream>
#include "crypto/aes.hpp"

namespace NeXo2::Loader {

namespace fs = std::filesystem;
using Crypto::Aes128;
using Crypto::Key128;

std::vector<u8> VFile::ReadAll() {
    std::vector<u8> data(static_cast<size_t>(Size()));
    data.resize(Read(0, data.data(), data.size()));
    return data;
}

namespace {

template <typename T> T Get(const u8* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }

class HostFile final : public VFile {
public:
    explicit HostFile(const fs::path& p) : m_file(p, std::ios::binary) {
        std::error_code ec;
        m_size = m_file ? fs::file_size(p, ec) : 0;
    }
    bool Ok() const { return bool(m_file); }
    u64 Size() const override { return m_size; }
    size_t Read(u64 offset, void* dst, size_t size) override {
        if (offset >= m_size) return 0;
        size = size_t(std::min<u64>(size, m_size - offset));
        std::lock_guard l(m_mutex);
        m_file.clear();
        m_file.seekg(std::streamoff(offset));
        m_file.read(static_cast<char*>(dst), std::streamsize(size));
        return size_t(m_file.gcount());
    }
private:
    std::ifstream m_file;
    u64 m_size = 0;
    std::mutex m_mutex;
};

class SubFile final : public VFile {
public:
    SubFile(VFilePtr base, u64 offset, u64 size) : m_base(std::move(base)), m_offset(offset), m_size(size) {}
    u64 Size() const override { return m_size; }
    size_t Read(u64 offset, void* dst, size_t size) override {
        if (offset >= m_size) return 0;
        return m_base->Read(m_offset + offset, dst, size_t(std::min<u64>(size, m_size - offset)));
    }
private:
    VFilePtr m_base;
    u64 m_offset, m_size;
};

class MemoryFile final : public VFile {
public:
    explicit MemoryFile(std::vector<u8> d) : m_data(std::move(d)) {}
    u64 Size() const override { return m_data.size(); }
    size_t Read(u64 offset, void* dst, size_t size) override {
        if (offset >= m_data.size()) return 0;
        size = size_t(std::min<u64>(size, m_data.size() - offset));
        std::memcpy(dst, m_data.data() + offset, size);
        return size;
    }
private:
    std::vector<u8> m_data;
};

// Seccion de una NCA cifrada con AES-CTR: se descifra al leer. 'start' = donde empieza
// la seccion en la NCA (el contador usa la posicion absoluta dentro de la NCA)
class CtrFile final : public VFile {
public:
    CtrFile(VFilePtr nca, u64 start, u64 size, const Key128& key, const u8 ctr_hi[8])
        : m_nca(std::move(nca)), m_start(start), m_size(size), m_aes(key) { std::memcpy(m_ctr, ctr_hi, 8); }
    u64 Size() const override { return m_size; }
    size_t Read(u64 offset, void* dst, size_t size) override {
        if (offset >= m_size) return 0;
        size = size_t(std::min<u64>(size, m_size - offset));
        const u64 abs = m_start + offset, aligned = abs & ~u64(15);
        const size_t lead = size_t(abs - aligned);
        std::vector<u8> buf(lead + size);
        const size_t got = m_nca->Read(aligned, buf.data(), buf.size());
        Crypto::AesCtrCrypt(m_aes, m_ctr, aligned, buf.data(), got);
        if (got <= lead) return 0;
        const size_t n = std::min(size, got - lead);
        std::memcpy(dst, buf.data() + lead, n);
        return n;
    }
private:
    VFilePtr m_nca;
    u64 m_start, m_size;
    Aes128 m_aes;
    u8 m_ctr[8];
};

bool ParseHex(const std::string& s, std::vector<u8>& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(u8(hi * 16 + lo));
    }
    return true;
}

std::string Hex(const u8* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// "nombre = valor" por linea
void ParseKeyFile(const fs::path& p, const std::function<void(const std::string&, const std::vector<u8>&)>& add) {
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find_first_of("=,");
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        const std::string name = Lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        std::vector<u8> bytes;
        if (!name.empty() && ParseHex(value, bytes)) add(name, bytes);
    }
}

// Claves de titulo de los tickets (.tik) de un NSP (solo los "comunes": los personales
// estan cifrados para una consola concreta)
void ReadTicket(VFilePtr tik, std::map<std::string, std::array<u8, 16>>& out) {
    std::vector<u8> t = tik->ReadAll();
    if (t.size() < 4) return;
    const u32 sig = Get<u32>(t.data());
    size_t data = 0;
    switch (sig) {
        case 0x10000: case 0x10003: data = 0x4 + 0x200 + 0x3C; break;   // RSA-4096
        case 0x10001: case 0x10004: data = 0x4 + 0x100 + 0x3C; break;   // RSA-2048
        case 0x10002: case 0x10005: data = 0x4 + 0x3C + 0x40; break;    // ECDSA
        default: return;
    }
    if (t.size() < data + 0x170) return;
    const u8 type = t[data + 0x141];   // 0 comun, 1 personal
    if (type != 0) return;
    std::array<u8, 16> key;
    std::memcpy(key.data(), &t[data + 0x40], 16);
    out[Hex(&t[data + 0x160], 16)] = key;
}
} // namespace

VFilePtr OpenHostFile(const fs::path& path) {
    auto f = std::make_shared<HostFile>(path);
    return f->Ok() ? f : nullptr;
}
VFilePtr MakeSubFile(VFilePtr base, u64 offset, u64 size) { return std::make_shared<SubFile>(std::move(base), offset, size); }
VFilePtr MakeMemoryFile(std::vector<u8> data) { return std::make_shared<MemoryFile>(std::move(data)); }

bool KeySet::Get(const std::string& name, std::array<u8, 16>& out) const {
    auto it = keys.find(name);
    if (it == keys.end() || it->second.size() != 16) return false;
    std::memcpy(out.data(), it->second.data(), 16);
    return true;
}
bool KeySet::GetHeaderKey(std::array<u8, 32>& out) const {
    auto it = keys.find("header_key");
    if (it == keys.end() || it->second.size() != 32) return false;
    std::memcpy(out.data(), it->second.data(), 32);
    return true;
}

bool LoadKeys(const std::vector<fs::path>& dirs, KeySet& out, std::string& found_in) {
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_regular_file(d / "prod.keys", ec)) continue;
        ParseKeyFile(d / "prod.keys", [&](const std::string& n, const std::vector<u8>& v) { out.keys[n] = v; });
        ParseKeyFile(d / "title.keys", [&](const std::string& n, const std::vector<u8>& v) {
            if (v.size() == 16) { std::array<u8, 16> k; std::memcpy(k.data(), v.data(), 16); out.title_keys[n] = k; }
        });
        auto u8p = d.u8string();
        found_in = std::string(u8p.begin(), u8p.end());
        return true;
    }
    return false;
}

bool ParsePartition(VFilePtr file, std::map<std::string, VFilePtr>& out) {
    u8 h[0x10];
    if (!file || file->Read(0, h, 0x10) != 0x10) return false;
    const bool hfs = std::memcmp(h, "HFS0", 4) == 0;
    if (!hfs && std::memcmp(h, "PFS0", 4) != 0) return false;
    const u32 count = Get<u32>(h + 4), strings = Get<u32>(h + 8);
    const u32 entry_size = hfs ? 0x40 : 0x18;
    if (count > 0x10000 || strings > 0x1000000) return false;
    std::vector<u8> table(size_t(count) * entry_size + strings);
    if (file->Read(0x10, table.data(), table.size()) != table.size()) return false;
    const u64 data_start = 0x10 + table.size();
    const char* names = reinterpret_cast<const char*>(table.data() + size_t(count) * entry_size);
    for (u32 i = 0; i < count; ++i) {
        const u8* e = table.data() + size_t(i) * entry_size;
        const u64 off = Get<u64>(e), size = Get<u64>(e + 8);
        const u32 name_off = Get<u32>(e + 16);
        if (name_off >= strings) continue;
        const std::string name(names + name_off, strnlen(names + name_off, strings - name_off));
        out[name] = MakeSubFile(file, data_start + off, size);
    }
    return true;
}

VFilePtr OpenRomfsFile(VFilePtr romfs, const std::string& path) {
    if (!romfs) return nullptr;
    u8 h[0x50];
    if (romfs->Read(0, h, 0x50) != 0x50) return nullptr;
    const u64 dir_meta = Get<u64>(h + 0x18), dir_meta_size = Get<u64>(h + 0x20);
    const u64 file_meta = Get<u64>(h + 0x38), file_meta_size = Get<u64>(h + 0x40), data = Get<u64>(h + 0x48);
    if (dir_meta_size > (64u << 20) || file_meta_size > (64u << 20)) return nullptr;
    std::vector<u8> dirs(static_cast<size_t>(dir_meta_size)), files(static_cast<size_t>(file_meta_size));
    romfs->Read(dir_meta, dirs.data(), dirs.size());
    romfs->Read(file_meta, files.data(), files.size());
    constexpr u32 NONE = 0xFFFFFFFF;
    auto name_of = [](const std::vector<u8>& tbl, u32 off, u32 name_at) {
        if (size_t(off) + name_at > tbl.size()) return std::string();
        const u32 len = Get<u32>(&tbl[off + name_at - 4]);
        if (size_t(off) + name_at + len > tbl.size()) return std::string();
        return std::string(reinterpret_cast<const char*>(&tbl[off + name_at]), len);
    };
    // Partir la ruta y bajar por las carpetas desde la raiz (entrada 0)
    std::vector<std::string> parts;
    std::stringstream ss(path);
    for (std::string p; std::getline(ss, p, '/');) if (!p.empty()) parts.push_back(p);
    if (parts.empty()) return nullptr;
    u32 dir = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (size_t(dir) + 0x18 > dirs.size()) return nullptr;
        u32 child = Get<u32>(&dirs[dir + 8]);
        while (child != NONE && size_t(child) + 0x18 <= dirs.size() && name_of(dirs, child, 0x18) != parts[i])
            child = Get<u32>(&dirs[child + 4]);
        if (child == NONE || size_t(child) + 0x18 > dirs.size()) return nullptr;
        dir = child;
    }
    if (size_t(dir) + 0x18 > dirs.size()) return nullptr;
    u32 f = Get<u32>(&dirs[dir + 12]);
    for (int guard = 0; f != NONE && size_t(f) + 0x20 <= files.size() && guard < 1000000; ++guard) {
        if (name_of(files, f, 0x20) == parts.back())
            return MakeSubFile(romfs, data + Get<u64>(&files[f + 8]), Get<u64>(&files[f + 16]));
        f = Get<u32>(&files[f + 4]);
    }
    return nullptr;
}

Nca OpenNca(VFilePtr file, const KeySet& keys, const std::map<std::string, std::array<u8, 16>>& extra) {
    Nca nca;
    std::array<u8, 32> hk;
    if (!keys.GetHeaderKey(hk)) { nca.error = "falta header_key en prod.keys"; return nca; }
    std::vector<u8> h(0xC00);
    if (file->Read(0, h.data(), h.size()) != h.size()) { nca.error = "NCA cortada"; return nca; }
    Key128 k1, k2;
    std::memcpy(k1.data(), hk.data(), 16);
    std::memcpy(k2.data(), hk.data() + 16, 16);
    Aes128 data_key(k1), tweak_key(k2);
    Crypto::AesXtsDecrypt(data_key, tweak_key, h.data(), 0x400, 0, 0x200);
    if (std::memcmp(&h[0x200], "NCA3", 4) == 0) {
        Crypto::AesXtsDecrypt(data_key, tweak_key, h.data() + 0x400, 0x800, 2, 0x200);
    } else if (std::memcmp(&h[0x200], "NCA2", 4) == 0) {
        for (int i = 0; i < 4; ++i) Crypto::AesXtsDecrypt(data_key, tweak_key, h.data() + 0x400 + 0x200 * i, 0x200, 0, 0x200);
    } else {
        nca.error = "cabecera de NCA no valida (header_key incorrecta?)";
        return nca;
    }
    nca.content_type = h[0x205];
    nca.program_id = Get<u64>(&h[0x210]);
    u32 gen = std::max<u32>(h[0x206], h[0x220]);
    if (gen > 0) --gen;
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%02x", gen);

    // Clave de las secciones: del ticket (rights id) o del area de claves de la cabecera
    Key128 ctr_key{};
    bool rights = false;
    for (int i = 0; i < 16; ++i) rights |= h[0x230 + i] != 0;
    if (rights) {
        const std::string rid = Hex(&h[0x230], 16);
        std::array<u8, 16> tk;
        auto it = extra.find(rid);
        if (it != extra.end()) tk = it->second;
        else if (auto t = keys.title_keys.find(rid); t != keys.title_keys.end()) tk = t->second;
        else { nca.error = "falta la clave del titulo (rights id " + rid + "): no hay ticket ni title.keys"; return nca; }
        Key128 kek;
        if (!keys.Get(std::string("titlekek_") + suffix, kek)) { nca.error = std::string("falta titlekek_") + suffix + " en prod.keys"; return nca; }
        Aes128(kek).DecryptEcb(tk.data(), 16);
        ctr_key = tk;
    } else {
        static const char* const kinds[3] = {"application", "ocean", "system"};
        const u8 idx = h[0x207];
        if (idx > 2) { nca.error = "indice de clave desconocido"; return nca; }
        const std::string name = std::string("key_area_key_") + kinds[idx] + "_" + suffix;
        Key128 kak;
        if (!keys.Get(name, kak)) { nca.error = "falta " + name + " en prod.keys"; return nca; }
        u8 area[0x40];
        std::memcpy(area, &h[0x300], 0x40);
        Aes128(kak).DecryptEcb(area, 0x40);
        std::memcpy(ctr_key.data(), area + 0x20, 16);   // la clave 2 es la de AES-CTR
    }

    for (int s = 0; s < 4; ++s) {
        const u64 start = u64(Get<u32>(&h[0x240 + 0x10 * s])) * 0x200;
        const u64 end = u64(Get<u32>(&h[0x244 + 0x10 * s])) * 0x200;
        if (end <= start) continue;
        const u8* fsh = &h[0x400 + 0x200 * s];
        const u8 fs_type = fsh[2], enc = fsh[4];
        u8 ctr_hi[8];
        for (int i = 0; i < 8; ++i) ctr_hi[i] = fsh[0x140 + 7 - i];
        VFilePtr section;
        if (enc == 1) section = MakeSubFile(file, start, end - start);
        else if (enc == 3) section = std::make_shared<CtrFile>(file, start, end - start, ctr_key, ctr_hi);
        else continue;   // XTS o BKTR (actualizaciones): todavia no
        if (fs_type == 1) {          // PFS0 (ExeFS)
            nca.exefs = MakeSubFile(section, Get<u64>(fsh + 0x40), Get<u64>(fsh + 0x48));
        } else if (fs_type == 0) {   // RomFS (IVFC: el nivel 6 son los datos)
            nca.romfs = MakeSubFile(section, Get<u64>(fsh + 0x90), Get<u64>(fsh + 0x98));
        }
    }
    nca.ok = true;
    return nca;
}

bool IsPackageFile(const fs::path& path) {
    const std::string ext = Lower(path.extension().string());
    return ext == ".nsp" || ext == ".xci";
}

GamePackage OpenGamePackage(const fs::path& path, const KeySet& keys, bool need_program) {
    GamePackage g;
    VFilePtr file = OpenHostFile(path);
    if (!file) { g.error = "no se puede abrir el archivo"; return g; }
    std::map<std::string, VFilePtr> files;
    const std::string ext = Lower(path.extension().string());
    if (ext == ".xci") {
        // Cabecera "HEAD" en 0x100 (o 0x1100 si el volcado lleva el area de claves delante)
        u64 base = 0;
        u8 magic[4];
        if (file->Read(0x100, magic, 4) != 4 || std::memcmp(magic, "HEAD", 4) != 0) {
            base = 0x1000;
            if (file->Read(0x1100, magic, 4) != 4 || std::memcmp(magic, "HEAD", 4) != 0) { g.error = "no es un XCI"; return g; }
        }
        u8 hdr[0x10];
        file->Read(base + 0x130, hdr, 0x10);
        const u64 root_off = Get<u64>(hdr), root_size = Get<u64>(hdr + 8);
        std::map<std::string, VFilePtr> root;
        if (!ParsePartition(MakeSubFile(file, base + root_off, root_size), root) || !root.count("secure")) {
            g.error = "XCI sin particion 'secure'";
            return g;
        }
        if (!ParsePartition(root["secure"], files)) { g.error = "particion 'secure' no valida"; return g; }
    } else if (!ParsePartition(file, files)) {
        g.error = "no es un NSP (falta la cabecera PFS0)";
        return g;
    }

    std::map<std::string, std::array<u8, 16>> ticket_keys;
    for (auto& [name, f] : files)
        if (Lower(name).size() > 4 && Lower(name).substr(name.size() - 4) == ".tik") ReadTicket(f, ticket_keys);

    std::string last_error;
    for (auto& [name, f] : files) {
        const std::string low = Lower(name);
        if (low.size() < 4 || low.substr(low.size() - 4) != ".nca" || low.find(".cnmt.") != std::string::npos) continue;
        Nca nca = OpenNca(f, keys, ticket_keys);
        if (!nca.ok) { last_error = name + ": " + nca.error; continue; }
        if (nca.content_type == 0 && nca.exefs && g.exefs.empty() && need_program) {
            std::map<std::string, VFilePtr> exefs;
            if (ParsePartition(nca.exefs, exefs) && exefs.count("main")) {
                g.exefs = std::move(exefs);
                g.romfs = nca.romfs;
                g.program_id = nca.program_id;
            } else {
                last_error = name + ": la seccion ExeFS no se pudo leer (claves incorrectas?)";
            }
        } else if (nca.content_type == 2 && nca.romfs && g.nacp.empty()) {
            if (auto n = OpenRomfsFile(nca.romfs, "/control.nacp")) g.nacp = n->ReadAll();
            for (const char* lang : {"AmericanEnglish", "BritishEnglish", "Spanish", "LatinAmericanSpanish", "Japanese"}) {
                if (auto ic = OpenRomfsFile(nca.romfs, std::string("/icon_") + lang + ".dat")) { g.icon = ic->ReadAll(); break; }
            }
        }
    }
    if (need_program && g.exefs.empty()) {
        g.error = last_error.empty() ? "no hay NCA de programa dentro" : last_error;
        return g;
    }
    if (!need_program && g.nacp.empty()) { g.error = last_error; return g; }
    g.ok = true;
    return g;
}

} // namespace NeXo2::Loader
