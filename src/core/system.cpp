#include "system.hpp"
#include "common/logger.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace NeXo2::Core {

using Common::Logger;

System::System() = default;

void System::ResetMachine() {
    m_memory.Clear();
    m_cpu.Reset();
    m_kernel.Reset();
    m_hasNro = false;
    m_nroInfo = {};
    m_lastError.clear();
}

bool System::LoadNroFile(const std::string& path) {
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
    return LoadNro(data, name);
}

bool System::LoadNro(const std::vector<u8>& data, const std::string& name) {
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
    m_lastRaw.clear();

    m_kernel.SetupHomebrewProcess(m_nroInfo.entry, m_nroInfo.base, m_nroInfo.image_size, name);
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

void System::Restart() {
    if (!m_lastNro.empty()) {
        const std::vector<u8> copy = m_lastNro; // LoadNro reemplaza m_lastNro
        LoadNro(copy, m_programName);
    } else if (!m_lastRaw.empty()) {
        const std::vector<u32> copy = m_lastRaw;
        LoadRawProgram(copy.data(), copy.size(), m_lastRawBase);
    }
}

} // namespace NeXo2::Core
