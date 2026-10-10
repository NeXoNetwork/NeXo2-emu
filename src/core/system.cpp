#include "system.hpp"
#include "common/logger.hpp"
#include <filesystem>
#include <fstream>
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

void System::LoadRawProgram(const u32* words, size_t count, u64 base) {
    ResetMachine();
    for (size_t i = 0; i < count; ++i) m_memory.Write<u32>(base + i * 4, words[i]);
    m_memory.MapRegion(base, (count * 4 + 0xFFF) & ~u64(0xFFF), MemoryState::Code,
                       MemoryPermission::ReadExecute, "demo");
    m_cpu.GetState().pc = base;
    m_programName = "demo";
    m_lastRaw.assign(words, words + count);
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
    if (!m_lastNro.empty()) {
        const std::vector<u8> copy = m_lastNro; // LoadNro reemplaza m_lastNro
        const std::string guest = m_guestPath, argv = m_lastArgv;
        LoadNro(copy, m_programName, guest, argv);
    } else if (!m_lastRaw.empty()) {
        const std::vector<u32> copy = m_lastRaw;
        LoadRawProgram(copy.data(), copy.size(), m_lastRawBase);
    }
}

} // namespace NeXo2::Core
