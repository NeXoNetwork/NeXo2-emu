#pragma once
#include <filesystem>
#include <fstream>
#include "hle/service.hpp"

// "fsp-srv": sistema de archivos (tarjeta SD, partidas guardadas, datos del juego...).
// Referencia: https://switchbrew.org/wiki/Filesystem_services
//
// La tarjeta SD ("sdmc:/") es una carpeta normal del PC (Kernel::GetSdmcRoot()).
// Las rutas de la Switch ("/config/app.ini") se traducen a rutas dentro de esa carpeta
// y nunca pueden salir de ella (".." se rechaza).
namespace NeXo2::HLE {

class FileSystemProxy final : public ServiceObject { public: FileSystemProxy(); };

// IFileSystem: crear/borrar/abrir archivos y carpetas
class FileSystem final : public ServiceObject {
public:
    explicit FileSystem(std::string name);
private:
    // Lee la ruta del buffer X 'index' y la convierte en ruta del PC. false si no es valida.
    bool HostPath(IpcContext& ctx, size_t index, std::filesystem::path& out);
};

// IFile: un archivo abierto
class FileObject final : public ServiceObject {
public:
    FileObject(std::filesystem::path path, u32 mode);
private:
    std::filesystem::path m_path;
    std::fstream m_file;
    u32 m_mode;  // bit 0 leer, bit 1 escribir, bit 2 ampliar al escribir
};

// IDirectory: lista de entradas de una carpeta
class DirectoryObject final : public ServiceObject {
public:
    DirectoryObject(const std::filesystem::path& path, u32 filter);
private:
    struct Entry { std::string name; bool is_dir; u64 size; };
    std::vector<Entry> m_entries;
    size_t m_next = 0;
};

// Normaliza una ruta de la Switch y la une a 'root'. Vacia si intenta salir de 'root'.
std::filesystem::path ResolveGuestPath(const std::filesystem::path& root, const std::string& guest);

} // namespace NeXo2::HLE
