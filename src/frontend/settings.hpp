#pragma once
// Ajustes de la aplicacion (ventana Opciones -> Ajustes). Se guardan en "nexo2.ini" junto
// al ejecutable, en texto: una linea "clave=valor" por ajuste ("game_dir" puede repetirse).
#include <string>
#include <vector>

namespace NeXo2::Frontend {

struct Settings {
    // General
    std::vector<std::string> game_dirs;   // carpetas donde buscar .nro (ademas de sdmc/switch)
    bool dev_mode = false;                // ventanas de depuracion (CPU, hilos, memoria...)
    bool grid_view = false;               // biblioteca en cuadricula (si no, lista)
    int  icon_size = 1;                   // 0 pequeno, 1 normal, 2 grande
    int  docked = 0;                      // modo de la consola: 0 portatil, 1 dock (TV)
    // CPU
    bool jit = true;
    bool multicore = true;
    bool decode_cache = true;
    // Graficos
    int  renderer = 1;                    // 0 software, 1 Vulkan (si no se puede, software)
    bool gpu_async = true;
    int  gpu_threads = 0;                 // hilos de dibujo por software (0 = todos los del PC)
    bool linear_filter = true;            // escalar la imagen suavizando (si no, pixeles nitidos)
    // Audio
    bool audio = true;
    int  volume = 100;                    // 0..100

    // Carga y guarda en 'path'. Load deja los valores por defecto si el archivo no existe.
    void Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace NeXo2::Frontend
