#pragma once
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace NeXo2::Common {

class Logger {
public:
    enum class Level { Info, Warning, Error, Debug };

    static void Log(Level level, const std::string& message) {
        if (Muted()) return;
        // La emulacion y la interfaz corren en hilos distintos: una linea cada vez
        std::lock_guard<std::mutex> lock(Lock());
        const char* prefix = "[INFO] ";
        switch (level) {
            case Level::Info:    prefix = "[INFO] ";  break;
            case Level::Warning: prefix = "[WARN] ";  break;
            case Level::Error:   prefix = "[ERR ] ";  break;
            case Level::Debug:   prefix = "[DEBUG] "; break;
        }
        // Errores a stderr, el resto a stdout (prefijo y mensaje en el mismo stream)
        std::ostream& out = (level == Level::Error) ? std::cerr : std::cout;
        out << prefix << message << '\n';
        // Copia en nexo2.log para poder revisarlo luego. Se vacia al disco en avisos y errores, y
        // como mucho cada medio segundo (vaciar en cada linea frenaba la emulacion).
        const auto now = std::chrono::steady_clock::now();
        const bool flush = level == Level::Warning || level == Level::Error || now - LastFlush() > std::chrono::milliseconds(500);
        if (std::ofstream* f = File()) {
            *f << prefix << message << '\n';
            if (flush) f->flush();
        }
        if (flush) { out.flush(); LastFlush() = now; }
    }

    // Vacia el archivo ya (al cerrar o antes de un fallo)
    static void Flush() {
        std::lock_guard<std::mutex> lock(Lock());
        if (std::ofstream* f = File()) f->flush();
        std::cout.flush();
    }

    // Activa la copia en archivo. Se llama una vez al arrancar (main.cpp).
    static void EnableFile(const char* path) {
        static std::ofstream file(path, std::ios::trunc);
        FileSlot() = file ? &file : nullptr;
    }

    // Silencia todo (lo usan los tests que ejecutan miles de instrucciones al azar)
    static void SetMuted(bool muted) { Muted() = muted; }

private:
    static bool& Muted() { static bool m = false; return m; }
    static std::mutex& Lock() { static std::mutex m; return m; }
    static std::ofstream*& FileSlot() { static std::ofstream* f = nullptr; return f; }
    static std::ofstream* File() { return FileSlot(); }
    static std::chrono::steady_clock::time_point& LastFlush() { static std::chrono::steady_clock::time_point t{}; return t; }
};

} // namespace NeXo2::Common