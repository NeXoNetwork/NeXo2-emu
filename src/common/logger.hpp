#pragma once
#include <fstream>
#include <iostream>
#include <string>

namespace NeXo2::Common {

class Logger {
public:
    enum class Level { Info, Warning, Error, Debug };

    static void Log(Level level, const std::string& message) {
        const char* prefix = "[INFO] ";
        switch (level) {
            case Level::Info:    prefix = "[INFO] ";  break;
            case Level::Warning: prefix = "[WARN] ";  break;
            case Level::Error:   prefix = "[ERR ] ";  break;
            case Level::Debug:   prefix = "[DEBUG] "; break;
        }
        // Errores a stderr, el resto a stdout (prefijo y mensaje en el mismo stream)
        std::ostream& out = (level == Level::Error) ? std::cerr : std::cout;
        out << prefix << message << std::endl;
        // Copia en nexo2.log (en la carpeta desde donde se ejecuta) para poder revisarlo luego
        if (std::ofstream* f = File()) *f << prefix << message << std::endl;
    }

    // Activa la copia en archivo. Se llama una vez al arrancar (main.cpp).
    static void EnableFile(const char* path) {
        static std::ofstream file(path, std::ios::trunc);
        FileSlot() = file ? &file : nullptr;
    }

private:
    static std::ofstream*& FileSlot() { static std::ofstream* f = nullptr; return f; }
    static std::ofstream* File() { return FileSlot(); }
};

} // namespace NeXo2::Common