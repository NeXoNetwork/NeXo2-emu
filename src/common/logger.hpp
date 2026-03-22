#pragma once
#include <iostream>
#include <string>

namespace NeXo2::Common {

class Logger {
public:
    enum class Level { Info, Warning, Error, Debug };

    static void Log(Level level, const std::string& message) {
        switch (level) {
            case Level::Info:    std::cout << "[INFO] "; break;
            case Level::Warning: std::cout << "[WARN] "; break;
            case Level::Error:   std::cerr << "[ERR ] "; break;
            case Level::Debug:   std::cout << "[DEBUG] "; break;
        }
        std::cout << message << std::endl;
    }
};

} // namespace NeXo2::Common