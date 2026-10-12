#pragma once
// Juegos en sus formatos de distribucion: NSP (tienda) y XCI (cartucho). Por dentro:
//
//   NSP = PFS0 (lista de archivos)  ->  varios .nca + .tik (ticket con la clave del titulo)
//   XCI = cabecera "HEAD" + HFS0    ->  particion "secure" (otro HFS0)  ->  varios .nca
//   NCA = cabecera de 0xC00 bytes cifrada (AES-XTS con header_key) + hasta 4 secciones
//         cifradas con AES-CTR. La clave de las secciones va en la propia cabecera, cifrada
//         con key_area_key_application_XX, o (si hay "rights id") es la clave del titulo del
//         ticket, cifrada con titlekek_XX. XX = generacion de claves de la NCA.
//         NCA de programa: seccion 0 = ExeFS (PFS0 con main, main.npdm...), 1 = RomFS.
//         NCA de control: RomFS con control.nacp e iconos.
//
// Las claves son las de la consola de cada uno: NeXo las lee de keys/prod.keys (y
// keys/title.keys) junto al ejecutable. NeXo no trae ninguna clave.
// Ver docs/07-nexo-internals/games.md.
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "common/types.hpp"

namespace NeXo2::Loader {

// "Archivo" de lectura: del disco, un trozo de otro, o descifrado al leer
class VFile {
public:
    virtual ~VFile() = default;
    virtual u64 Size() const = 0;
    // Lee hasta 'size' bytes desde 'offset'; devuelve los leidos
    virtual size_t Read(u64 offset, void* dst, size_t size) = 0;
    std::vector<u8> ReadAll();
};
using VFilePtr = std::shared_ptr<VFile>;

VFilePtr OpenHostFile(const std::filesystem::path& path);
VFilePtr MakeSubFile(VFilePtr base, u64 offset, u64 size);
VFilePtr MakeMemoryFile(std::vector<u8> data);

// Claves (prod.keys / title.keys: lineas "nombre = hex")
struct KeySet {
    std::map<std::string, std::vector<u8>> keys;
    std::map<std::string, std::array<u8, 16>> title_keys;   // rights id (hex) -> clave del titulo (cifrada)
    bool Get(const std::string& name, std::array<u8, 16>& out) const;
    bool GetHeaderKey(std::array<u8, 32>& out) const;
};
// Lee prod.keys y title.keys de la primera carpeta que los tenga. false si no hay prod.keys.
bool LoadKeys(const std::vector<std::filesystem::path>& dirs, KeySet& out, std::string& found_in);

// PFS0 (NSP, ExeFS) y HFS0 (XCI): nombre -> archivo
bool ParsePartition(VFilePtr file, std::map<std::string, VFilePtr>& out);

// RomFS: un archivo por su ruta ("/control.nacp"); nullptr si no esta
VFilePtr OpenRomfsFile(VFilePtr romfs, const std::string& path);

struct Nca {
    bool ok = false;
    std::string error;
    u8 content_type = 0;            // 0 programa, 1 meta, 2 control, 3 manual, 4 datos
    u64 program_id = 0;
    VFilePtr exefs;                 // PFS0 de la seccion de ExeFS (programa)
    VFilePtr romfs;                 // datos de la RomFS (nivel 6 de IVFC)
};
// 'extra_title_keys' = claves de titulo sacadas de los tickets del NSP
Nca OpenNca(VFilePtr file, const KeySet& keys, const std::map<std::string, std::array<u8, 16>>& extra_title_keys);

// Lo que hace falta para arrancar un juego
struct GamePackage {
    bool ok = false;
    std::string error;
    std::map<std::string, VFilePtr> exefs;   // main, main.npdm, rtld, subsdk*, sdk
    VFilePtr romfs;
    std::vector<u8> nacp;                    // control.nacp (vacio si no hay)
    std::vector<u8> icon;                    // icono JPEG (vacio si no hay)
    u64 program_id = 0;
};
// .nsp o .xci. 'need_program' = false: solo el control (nombre e icono, para la biblioteca)
GamePackage OpenGamePackage(const std::filesystem::path& path, const KeySet& keys, bool need_program = true);
bool IsPackageFile(const std::filesystem::path& path);   // por la extension: .nsp, .xci

} // namespace NeXo2::Loader
