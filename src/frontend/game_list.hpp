#pragma once
// Biblioteca de juegos: busca los .nro (y los juegos extraidos) de las carpetas elegidas y lee de cada uno su
// nombre, autor, version e icono (del NACP y el JPEG que van al final del archivo, en
// la seccion "ASET"). No carga el programa: solo lee esas partes del archivo.
#include <filesystem>
#include <string>
#include <vector>
#include "common/types.hpp"

namespace NeXo2::Frontend {

struct GameEntry {
    std::string path;        // ruta completa (UTF-8)
    std::string file_name;   // nombre del archivo
    std::string title;       // del NACP (o el nombre del archivo si no hay)
    std::string author;
    std::string version;
    u64 size = 0;            // bytes
    std::vector<u8> icon;    // RGBA (vacio si no tiene)
    int icon_w = 0, icon_h = 0;
    void* texture = nullptr; // textura del icono (la crea y destruye la interfaz)
    bool is_folder = false;  // juego extraido (carpeta con ExeFS) en vez de un .nro
};

// Lee la informacion de un .nro. Devuelve false si no es un NRO valido.
bool ReadNroMetadata(const std::filesystem::path& file, GameEntry& out);

// Juego extraido: carpeta con exefs/main (o main + main.npdm). Lee control.nacp e icon_*.dat.
bool IsGameDirectory(const std::filesystem::path& dir);
bool ReadGameFolderMetadata(const std::filesystem::path& dir, GameEntry& out);

// Busca .nro y juegos extraidos en las carpetas (y subcarpetas, hasta 3 niveles). Sin repetidos.
std::vector<GameEntry> ScanGames(const std::vector<std::filesystem::path>& dirs);

// Conversiones de ruta <-> texto UTF-8 (Windows usa UTF-16 por dentro)
std::string PathToUtf8(const std::filesystem::path& p);
std::filesystem::path Utf8ToPath(const std::string& s);

} // namespace NeXo2::Frontend
