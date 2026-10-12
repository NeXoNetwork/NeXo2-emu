#include "game_list.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace fs = std::filesystem;

namespace NeXo2::Frontend {

std::string PathToUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
fs::path Utf8ToPath(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

namespace {
template <typename T> T Get(const u8* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }

std::string CString(const u8* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

bool ReadAt(std::ifstream& f, u64 offset, void* dst, size_t size) {
    f.clear();
    f.seekg(std::streamoff(offset));
    f.read(static_cast<char*>(dst), std::streamsize(size));
    return size_t(f.gcount()) == size;
}
// NACP: 16 idiomas de 0x300 bytes (nombre 0x200 + autor 0x100); version en 0x3060
void ParseNacp(const u8* nacp, GameEntry& out) {
    // Preferencia: espanol (6), espanol latinoamericano (5), ingles (0), el primero que haya
    const int order[] = {6, 5, 0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int lang : order) {
        const u8* e = nacp + lang * 0x300;
        std::string name = CString(e, 0x200);
        if (name.empty()) continue;
        out.title = name;
        out.author = CString(e + 0x200, 0x100);
        break;
    }
    out.version = CString(nacp + 0x3060, 0x10);
}

void DecodeIcon(const std::vector<u8>& jpeg, GameEntry& out) {
    int w = 0, h = 0, n = 0;
    if (u8* px = stbi_load_from_memory(jpeg.data(), int(jpeg.size()), &w, &h, &n, 4)) {
        out.icon.assign(px, px + size_t(w) * size_t(h) * 4);
        out.icon_w = w;
        out.icon_h = h;
        stbi_image_free(px);
    }
}
} // namespace

bool ReadNroMetadata(const fs::path& file, GameEntry& out) {
    std::error_code ec;
    out.size = fs::file_size(file, ec);
    if (ec) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    u8 head[0x80];
    if (!ReadAt(f, 0, head, sizeof(head)) || std::memcmp(head + 0x10, "NRO0", 4) != 0) return false;
    out.path = PathToUtf8(file);
    out.file_name = PathToUtf8(file.filename());
    out.title = PathToUtf8(file.stem());

    // Assets: justo despues de la imagen ('size' en la cabecera)
    const u32 image_size = Get<u32>(head + 0x18);
    u8 aset[0x38];
    if (!ReadAt(f, image_size, aset, sizeof(aset)) || std::memcmp(aset, "ASET", 4) != 0) return true;
    const u64 icon_off = Get<u64>(aset + 0x08), icon_size = Get<u64>(aset + 0x10);
    const u64 nacp_off = Get<u64>(aset + 0x18), nacp_size = Get<u64>(aset + 0x20);

    // NACP: 16 idiomas de 0x300 bytes (nombre 0x200 + autor 0x100); version en 0x3060
    if (nacp_size >= 0x4000) {
        std::vector<u8> nacp(0x4000);
        if (ReadAt(f, image_size + nacp_off, nacp.data(), nacp.size())) ParseNacp(nacp.data(), out);
    }
    // Icono: JPEG (normalmente 256x256)
    if (icon_size > 0 && icon_size < (4u << 20)) {
        std::vector<u8> jpeg(static_cast<size_t>(icon_size), u8(0));
        if (ReadAt(f, image_size + icon_off, jpeg.data(), jpeg.size())) DecodeIcon(jpeg, out);
    }
    return true;
}

bool IsGameDirectory(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "exefs" / "main", ec) || fs::is_regular_file(dir / "ExeFS" / "main", ec)) return true;
    return fs::is_regular_file(dir / "main", ec) && fs::is_regular_file(dir / "main.npdm", ec);
}

bool ReadGameFolderMetadata(const fs::path& dir, GameEntry& out) {
    if (!IsGameDirectory(dir)) return false;
    std::error_code ec;
    out.path = PathToUtf8(dir);
    out.file_name = PathToUtf8(dir.filename());
    out.title = out.file_name;
    out.is_folder = true;
    out.size = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        if (e.is_regular_file(ec)) out.size += e.file_size(ec);
    // control.nacp e icono (icon_<idioma>.dat, un JPEG), como los deja hactool al extraer el control
    std::ifstream f(dir / "control.nacp", std::ios::binary);
    std::vector<u8> nacp(0x4000);
    if (f && ReadAt(f, 0, nacp.data(), nacp.size())) ParseNacp(nacp.data(), out);
    fs::path icon;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = PathToUtf8(e.path().filename());
        if (name.rfind("icon_", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".dat") {
            if (icon.empty() || name == "icon_AmericanEnglish.dat") icon = e.path();
        }
    }
    if (!icon.empty()) {
        std::ifstream fi(icon, std::ios::binary);
        std::vector<u8> jpeg((std::istreambuf_iterator<char>(fi)), std::istreambuf_iterator<char>());
        if (!jpeg.empty() && jpeg.size() < (4u << 20)) DecodeIcon(jpeg, out);
    }
    return true;
}

bool ReadPackageMetadata(const fs::path& file, const Loader::KeySet* keys, GameEntry& out) {
    std::error_code ec;
    out.path = PathToUtf8(file);
    out.file_name = PathToUtf8(file.filename());
    out.title = PathToUtf8(file.stem());
    out.size = fs::file_size(file, ec);
    out.is_folder = true;   // se carga con System::LoadGame
    if (!keys) return true;
    auto pkg = Loader::OpenGamePackage(file, *keys, false);
    if (pkg.nacp.size() >= 0x4000) ParseNacp(pkg.nacp.data(), out);
    if (!pkg.icon.empty() && pkg.icon.size() < (4u << 20)) DecodeIcon(pkg.icon, out);
    return true;
}

std::vector<GameEntry> ScanGames(const std::vector<fs::path>& dirs, const Loader::KeySet* keys) {
    std::vector<GameEntry> games;
    std::set<fs::path> seen;
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        if (IsGameDirectory(dir)) {          // la carpeta elegida es directamente un juego
            GameEntry g;
            if (seen.insert(dir).second && ReadGameFolderMetadata(dir, g)) games.push_back(std::move(g));
            continue;
        }
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            const auto& p = it->path();
            std::error_code ec2;
            if (it->is_directory(ec2) && IsGameDirectory(p)) {   // juego extraido (ExeFS)
                it.disable_recursion_pending();
                const fs::path canon = fs::weakly_canonical(p, ec2);
                if (!seen.insert(ec2 ? p : canon).second) continue;
                GameEntry g;
                if (ReadGameFolderMetadata(p, g)) games.push_back(std::move(g));
                continue;
            }
            if (!it->is_regular_file(ec2)) continue;
            auto ext = PathToUtf8(p.extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext != ".nro" && ext != ".nsp" && ext != ".xci") continue;
            const fs::path canon = fs::weakly_canonical(p, ec2);
            if (!seen.insert(ec2 ? p : canon).second) continue;
            GameEntry g;
            if (ext == ".nro" ? ReadNroMetadata(p, g) : ReadPackageMetadata(p, keys, g)) games.push_back(std::move(g));
        }
    }
    std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
        std::string x = a.title, y = b.title;
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return x < y;
    });
    return games;
}

} // namespace NeXo2::Frontend
