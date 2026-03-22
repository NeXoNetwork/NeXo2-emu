cmake_minimum_required(VERSION 3.20)
project(NeXo2 
    VERSION 0.0.1 
    DESCRIPTION "Nintendo Switch 2 Emulation Research Laboratory"
    LANGUAGES CXX
)

# 1. Configuración de C++
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 2. Carpetas de inclusión (Para que los #include funcionen)
include_directories(src)

# 3. Definición de archivos fuente (Asegúrate de que existan)
set(SOURCES
    src/main.cpp
    src/common/logger.hpp
    src/core/memory/memory.hpp
    src/core/arm64/cpu_state.hpp
    src/core/arm64/interpreter.hpp
    src/core/arm64/interpreter.cpp
)

# 4. Dependencias (SDL3)
# EXCLUDE_FROM_ALL evita que se compilen ejemplos innecesarios de SDL
add_subdirectory(externals/SDL EXCLUDE_FROM_ALL)

# 5. Crear el Ejecutable
add_executable(${PROJECT_NAME} ${SOURCES})

# 6. Enlace de Librerías (Usando SDL3 según tu log anterior)
target_link_libraries(${PROJECT_NAME} 
    PRIVATE 
        SDL3::SDL3-shared
)

# 7. Organizar salida de archivos
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "NeXo 2 Project configured for SDL3 successfully!")