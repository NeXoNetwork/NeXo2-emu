#include "system.hpp"
#include "common/logger.hpp"
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <map>
#include <iterator>

namespace NeXo2::Core {

using Common::Logger;

System::System() = default;

void System::ResetMachine() {
    m_kernel.GetGpu().WaitIdle();   // antes de borrar la memoria que la GPU podria estar leyendo
    m_memory.Clear();
    m_cpu.Reset();
    m_kernel.Reset();
    m_hasNro = false;
    m_nroInfo = {};
    m_lastError.clear();
}

bool System::LoadNroFile(const std::string& path, const std::string& argv) {
    // La ruta llega en UTF-8 (SDL, linea de comandos...). Asi tambien funcionan
    // carpetas con tildes o enes en Windows.
    const std::u8string utf8_path(path.begin(), path.end());
    std::ifstream file(std::filesystem::path(utf8_path), std::ios::binary);
    if (!file) {
        m_lastError = "No se puede abrir el archivo: " + path;
        Logger::Log(Logger::Level::Error, "[Loader] " + m_lastError);
        return false;
    }
    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // argv = solo el nombre del archivo, como lo veria el homebrew
    const size_t slash = path.find_last_of("/\\");
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    // Si el .nro esta dentro de la carpeta de la SD, el programa lo ve en su sitio de verdad
    // (como al lanzarlo desde el hbmenu). Asi su carpeta de trabajo es la suya y encuentra los
    // archivos que lleva al lado (datos de los ports, configuracion...).
    std::string guest;
    {
        std::error_code ec;
        const auto file = std::filesystem::weakly_canonical(std::filesystem::path(utf8_path), ec);
        const auto root = std::filesystem::weakly_canonical(m_kernel.GetSdmcRoot(), ec);
        const auto rel = file.lexically_relative(root);
        if (!ec && !rel.empty() && *rel.begin() != "..") {
            const auto u8rel = rel.generic_u8string();
            guest = "/" + std::string(u8rel.begin(), u8rel.end());
        }
    }
    m_currentFile = path;
    return LoadNro(data, name, guest, argv);
}

bool System::LoadNro(const std::vector<u8>& data, const std::string& name, const std::string& guest_path,
                     const std::string& argv) {
    ResetMachine();

    Loader::NroLoadResult result = Loader::LoadNro(data, m_memory, HLE::Layout::CODE_BASE);
    if (!result.ok) {
        m_lastError = "NRO no valido: " + result.error;
        Logger::Log(Logger::Level::Error, "[Loader] " + m_lastError);
        return false;
    }

    m_nroInfo = result.info;
    m_hasNro = true;
    m_programName = name;
    m_lastNro = data;
    m_lastArgv = argv;
    m_lastRaw.clear();
    m_lastGame.clear();

    // Como el hbmenu: argv[0] = ruta del .nro en la SD. El .nro se ve en esa ruta aunque no
    // este de verdad en la SD, para que romfsInit() pueda leer su RomFS.
    const std::string guest = guest_path.empty() ? "/switch/" + name : guest_path;
    m_guestPath = guest_path;
    const std::string argv0 = "sdmc:" + guest;
    m_kernel.SetSelfNro(guest, data);
    m_kernel.SetupHomebrewProcess(m_nroInfo.entry, m_nroInfo.base, m_nroInfo.image_size,
                                  !argv.empty() ? argv
                                  : argv0.find(' ') == std::string::npos ? argv0 : "\"" + argv0 + "\"");
    Logger::Log(Logger::Level::Info, "[Loader] NRO cargado: " + name +
                (m_nroInfo.title.empty() ? "" : " (" + m_nroInfo.title + ")"));
    return true;
}

namespace {
std::filesystem::path Utf8Path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

bool ReadWholeFile(const std::filesystem::path& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Carpeta del ExeFS de un juego extraido ('path' = la carpeta del juego, la del ExeFS o un
// archivo de dentro). Vacia si no hay un "main".
std::filesystem::path FindExefs(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = Utf8Path(path);
    if (!fs::is_directory(dir, ec)) dir = dir.parent_path();
    for (const fs::path& d : {dir / "exefs", dir / "ExeFS", dir})
        if (fs::is_regular_file(d / "main", ec)) return d;
    return {};
}

// Nombre del juego en control.nacp (primer idioma que lo tenga)
std::string NacpTitle(const std::vector<u8>& nacp) {
    if (nacp.size() < 0x3000) return {};
    for (int lang = 0; lang < 16; ++lang) {
        std::string name;
        for (size_t i = 0; i < 0x200 && nacp[lang * 0x300 + i]; ++i) name += char(nacp[lang * 0x300 + i]);
        if (!name.empty()) return name;
    }
    return {};
}
} // namespace

bool System::IsGameFolder(const std::string& path) {
    return Loader::IsPackageFile(Utf8Path(path)) || !FindExefs(path).empty();
}

std::vector<std::filesystem::path> System::KeyDirs() const {
    // keys/ junto al ejecutable (la carpeta sdmc esta ahi) o al lado de sdmc; NEXO2_KEYS manda
    std::vector<std::filesystem::path> dirs;
    if (const char* e = std::getenv("NEXO2_KEYS")) dirs.push_back(Utf8Path(e));
    const auto base = m_kernel.GetSdmcRoot().parent_path();
    dirs.push_back(base / "keys");
    dirs.push_back(base.empty() ? std::filesystem::path(".") : base);
    return dirs;
}

bool System::LoadGame(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto fail = [&](const std::string& why) {
        m_lastError = why;
        Logger::Log(Logger::Level::Error, "[Loader] " + m_lastError);
        return false;
    };

    // --- De donde salen los archivos: un NSP/XCI o una carpeta extraida ---
    std::map<std::string, Loader::VFilePtr> exefs;
    Loader::VFilePtr romfs;
    std::vector<u8> nacp;
    std::string fallback_name;
    u64 package_program_id = 0;
    if (Loader::IsPackageFile(Utf8Path(path))) {
        Loader::KeySet keys;
        std::string where;
        if (!Loader::LoadKeys(KeyDirs(), keys, where)) {
            const auto dir = KeyDirs()[std::getenv("NEXO2_KEYS") ? 1 : 0].u8string();
            return fail("Para abrir NSP y XCI hacen falta las claves de TU consola: copia tu prod.keys (y title.keys "
                        "si lo tienes) en " + std::string(dir.begin(), dir.end()));
        }
        auto pkg = Loader::OpenGamePackage(Utf8Path(path), keys);
        if (!pkg.ok) return fail("No se pudo abrir " + path + ": " + pkg.error);
        exefs = std::move(pkg.exefs);
        romfs = pkg.romfs;
        nacp = std::move(pkg.nacp);
        package_program_id = pkg.program_id;
        fallback_name = Utf8Path(path).stem().string();
        Logger::Log(Logger::Level::Info, "[Loader] Claves leidas de " + where);
    } else {
        const fs::path dir_exefs = FindExefs(path);
        if (dir_exefs.empty()) return fail("No es un juego extraido (falta el archivo 'main' del ExeFS): " + path);
        const fs::path dir = (dir_exefs.filename() == "exefs" || dir_exefs.filename() == "ExeFS") ? dir_exefs.parent_path() : dir_exefs;
        for (const auto& e : fs::directory_iterator(dir_exefs, ec))
            if (e.is_regular_file(ec)) exefs[e.path().filename().string()] = Loader::OpenHostFile(e.path());
        for (const fs::path& r : {dir / "romfs.bin", dir_exefs / "romfs.bin"})
            if (fs::is_regular_file(r, ec)) { romfs = Loader::OpenHostFile(r); break; }
        std::vector<u8> d;
        if (ReadWholeFile(dir / "control.nacp", d)) nacp = std::move(d);
        fallback_name = dir.filename().string();
    }

    Loader::NpdmInfo npdm;
    if (auto it = exefs.find("main.npdm"); it != exefs.end() && it->second) {
        auto r = Loader::ParseNpdm(it->second->ReadAll());
        if (!r.ok) return fail(r.error);
        npdm = r.info;
    } else {
        Logger::Log(Logger::Level::Warning, "[Loader] No hay main.npdm: se usan valores por defecto");
    }
    if (!npdm.program_id) npdm.program_id = package_program_id;

    ResetMachine();
    // Los modulos van seguidos en memoria, en este orden; se empieza por el primero
    std::vector<std::string> names = {"rtld", "main"};
    for (int i = 0; i < 10; ++i) names.push_back("subsdk" + std::to_string(i));
    names.push_back("sdk");
    u64 base = HLE::Layout::CODE_BASE, entry = 0;
    std::string loaded;
    for (const auto& name : names) {
        auto it = exefs.find(name);
        if (it == exefs.end() || !it->second) continue;
        auto r = Loader::LoadNso(it->second->ReadAll(), m_memory, base, name);
        if (!r.ok) return fail("NSO no valido: " + r.error);
        if (!entry) entry = base;
        base += r.image_size;
        loaded += (loaded.empty() ? "" : ", ") + name;
    }
    if (!entry) return fail("El juego no tiene codigo (ExeFS sin 'main')");

    const std::string title = NacpTitle(nacp);
    m_programName = !title.empty() ? title : !npdm.name.empty() ? npdm.name : fallback_name;
    m_nroInfo = {};
    m_nroInfo.base = HLE::Layout::CODE_BASE;
    m_nroInfo.entry = entry;
    m_nroInfo.image_size = base - HLE::Layout::CODE_BASE;
    m_nroInfo.title = title;
    m_hasNro = true;
    m_lastNro.clear();
    m_lastRaw.clear();
    m_lastGame = path;
    m_currentFile = path;

    m_kernel.SetRomfs(romfs);
    m_kernel.SetupApplicationProcess(entry, m_nroInfo.image_size, npdm.main_priority, npdm.main_core,
                                     npdm.main_stack_size, npdm.program_id);
    Logger::Log(Logger::Level::Info, "[Loader] Juego cargado: " + m_programName + " (modulos: " + loaded + ")" +
                (romfs ? ", con RomFS" : ", sin RomFS"));
    return true;
}

void System::LoadRawProgram(const u32* words, size_t count, u64 base) {
    ResetMachine();
    for (size_t i = 0; i < count; ++i) m_memory.Write<u32>(base + i * 4, words[i]);
    m_memory.MapRegion(base, (count * 4 + 0xFFF) & ~u64(0xFFF), MemoryState::Code,
                       MemoryPermission::ReadExecute, "demo");
    m_cpu.GetState().pc = base;
    m_programName = "demo";
    m_lastRaw.assign(words, words + count);
    m_lastGame.clear();
    m_lastRawBase = base;
    m_lastNro.clear();
}

bool System::ContinueAfterExit() {
    if (!m_kernel.HasExited()) return false;
    const std::string next = m_kernel.GetNextLoadPath();
    const std::string argv = m_kernel.GetNextLoadArgv();
    if (!next.empty()) {
        // "sdmc:/switch/juego.nro" -> archivo dentro de la carpeta de la SD
        std::string guest = next;
        if (guest.rfind("sdmc:", 0) == 0) guest = guest.substr(5);
        if (guest.empty() || guest[0] != '/') guest = "/" + guest;
        const auto host = (m_kernel.GetSdmcRoot() / std::filesystem::path(std::u8string(guest.begin() + 1, guest.end())));
        const auto u8host = host.u8string();
        const std::string menu = m_currentFile;
        Logger::Log(Logger::Level::Info, "[Loader] El programa pide cargar " + next + " (argv: " + argv + ")");
        if (!LoadNroFile(std::string(u8host.begin(), u8host.end()), argv)) return false;
        m_menuPath = menu;
        return true;
    }
    if (!m_menuPath.empty() && m_menuPath != m_currentFile) {
        Logger::Log(Logger::Level::Info, "[Loader] Fin del programa: vuelta al menu");
        const std::string menu = m_menuPath;
        m_menuPath.clear();
        return LoadNroFile(menu);
    }
    return false;
}

void System::Restart() {
    if (!m_lastGame.empty()) {
        const std::string game = m_lastGame;
        LoadGame(game);
    } else if (!m_lastNro.empty()) {
        const std::vector<u8> copy = m_lastNro; // LoadNro reemplaza m_lastNro
        const std::string guest = m_guestPath, argv = m_lastArgv;
        LoadNro(copy, m_programName, guest, argv);
    } else if (!m_lastRaw.empty()) {
        const std::vector<u32> copy = m_lastRaw;
        LoadRawProgram(copy.data(), copy.size(), m_lastRawBase);
    }
}

} // namespace NeXo2::Core
