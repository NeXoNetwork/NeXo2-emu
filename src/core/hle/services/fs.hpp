#pragma once
#include "hle/service.hpp"

// "fsp-srv": sistema de archivos (tarjeta SD, partidas guardadas, datos del juego...).
// Referencia: https://switchbrew.org/wiki/Filesystem_services
namespace NeXo2::HLE {

class FileSystemProxy final : public ServiceObject { public: FileSystemProxy(); };

// IFileSystem: lo que devuelve OpenSdCardFileSystem. Sin comandos todavia:
// abrir/leer archivos sera el siguiente paso (y la CPU se parara diciendo cual falta).
class FileSystem final : public ServiceObject {
public:
    explicit FileSystem(std::string name) : ServiceObject(std::move(name)) {}
};

} // namespace NeXo2::HLE
