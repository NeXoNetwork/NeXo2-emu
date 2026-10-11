#include "settings.hpp"
#include <algorithm>
#include <fstream>

namespace NeXo2::Frontend {

namespace {
bool ToBool(const std::string& v) { return v == "1" || v == "true" || v == "on"; }
int ToInt(const std::string& v, int def) {
    try { return std::stoi(v); } catch (...) { return def; }
}
} // namespace

void Settings::Load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    game_dirs.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "game_dir") { if (!v.empty()) game_dirs.push_back(v); }
        else if (k == "dev_mode") dev_mode = ToBool(v);
        else if (k == "grid_view") grid_view = ToBool(v);
        else if (k == "icon_size") icon_size = std::clamp(ToInt(v, 1), 0, 2);
        else if (k == "jit") jit = ToBool(v);
        else if (k == "multicore") multicore = ToBool(v);
        else if (k == "decode_cache") decode_cache = ToBool(v);
        else if (k == "gpu_async") gpu_async = ToBool(v);
        else if (k == "gpu_threads") gpu_threads = std::clamp(ToInt(v, 0), 0, 64);
        else if (k == "linear_filter") linear_filter = ToBool(v);
        else if (k == "audio") audio = ToBool(v);
        else if (k == "volume") volume = std::clamp(ToInt(v, 100), 0, 100);
    }
}

bool Settings::Save(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# Ajustes de NeXo 2 (se pueden cambiar en Opciones -> Ajustes)\n";
    for (const auto& d : game_dirs) f << "game_dir=" << d << "\n";
    f << "dev_mode=" << dev_mode << "\n"
      << "grid_view=" << grid_view << "\n"
      << "icon_size=" << icon_size << "\n"
      << "jit=" << jit << "\n"
      << "multicore=" << multicore << "\n"
      << "decode_cache=" << decode_cache << "\n"
      << "gpu_async=" << gpu_async << "\n"
      << "gpu_threads=" << gpu_threads << "\n"
      << "linear_filter=" << linear_filter << "\n"
      << "audio=" << audio << "\n"
      << "volume=" << volume << "\n";
    return bool(f);
}

} // namespace NeXo2::Frontend
