#pragma once
#include <memory>
#include <string>
#include <vector>
#include "common/types.hpp"
#include "memory.hpp"
#include "arm64/interpreter.hpp"
#include "hle/kernel.hpp"
#include "loader/nro.hpp"
#include "loader/npdm.hpp"
#include "loader/nso.hpp"
#include "loader/package.hpp"

// "La consola": junta memoria, CPU y kernel HLE, y sabe cargar programas.
// La interfaz (main.cpp) y los tests solo hablan con esta clase.
namespace NeXo2::Core {

class System {
public:
    System();

    // Carga un NRO desde disco. Devuelve false y deja el motivo en GetLastError().
    // 'argv' vacio = la ruta del .nro en la SD (como lo lanza el hbmenu sin argumentos).
    bool LoadNroFile(const std::string& path, const std::string& argv = "");

    // Carga un NRO ya leido en memoria ('name' se usa como argv). 'guest_path' = donde esta
    // el .nro en la SD ("/switch/juego/juego.nro"); vacio = "/switch/<name>".
    bool LoadNro(const std::vector<u8>& data, const std::string& name, const std::string& guest_path = "",
                 const std::string& argv = "");

    // Carga un juego "extraido": una carpeta con su ExeFS (main, main.npdm, rtld, subsdk*, sdk;
    // directamente o en una subcarpeta "exefs") y opcionalmente romfs.bin y control.nacp.
    // 'path' puede ser la carpeta o cualquier archivo de dentro. Ver docs/07-nexo-internals/games.md
    bool LoadGame(const std::string& path);
    // Tambien .nsp y .xci, con las claves de la consola del usuario (keys/prod.keys).
    // true si 'path' es algo que acepta LoadGame (carpeta extraida, .nsp o .xci)
    static bool IsGameFolder(const std::string& path);
    // Carpetas donde se buscan prod.keys y title.keys
    std::vector<std::filesystem::path> KeyDirs() const;

    // Carga instrucciones sueltas en 'base' (la demo de main.cpp y algunos tests).
    void LoadRawProgram(const u32* words, size_t count, u64 base);

    // Vuelve a cargar el ultimo programa desde el principio.
    void Restart();

    // Despues de que el programa termine, como hace el loader de homebrew: si pidio cargar
    // otro (envSetNextLoad: el hbmenu al elegir un juego), lo carga; si termino sin pedir
    // nada y lo habia lanzado el menu, vuelve al menu. Devuelve true si cargo algo.
    bool ContinueAfterExit();

    // Ejecuta hasta 'budget' instrucciones (repartidas entre los hilos del programa).
    // Usar esto en vez de GetCpu().Run(): el planificador del kernel decide que hilo corre.
    u64 Run(u64 budget) { return m_kernel.Run(budget); }

    Memory&       GetMemory() { return m_memory; }
    Interpreter&  GetCpu()    { return m_cpu; }
    HLE::Kernel&  GetKernel() { return m_kernel; }

    const std::string& GetLastError() const { return m_lastError; }
    // Informacion del NRO cargado (nullptr si no hay ninguno).
    const Loader::NroInfo* GetNroInfo() const { return m_hasNro ? &m_nroInfo : nullptr; }
    const std::string& GetProgramName() const { return m_programName; }

private:
    void ResetMachine();

    Memory      m_memory;
    Interpreter m_cpu{m_memory};
    HLE::Kernel m_kernel{m_memory, m_cpu};

    std::string     m_lastError;
    std::string     m_programName;
    std::string     m_guestPath;     // ruta del .nro dentro de la SD (para Restart)
    std::string     m_menuPath;      // archivo del menu que lanzo este programa (vacio = ninguno)
    std::string     m_currentFile;   // archivo del programa cargado
    std::string     m_lastArgv;      // argv que se le dio (para Restart)
    std::string     m_lastGame;      // carpeta del juego cargado con LoadGame (para Restart)
    bool            m_hasNro = false;
    Loader::NroInfo m_nroInfo;

    // Copia del ultimo programa, para Restart()
    std::vector<u8>  m_lastNro;
    std::vector<u32> m_lastRaw;
    u64              m_lastRawBase = 0;
};

} // namespace NeXo2::Core
