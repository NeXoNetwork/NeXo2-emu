cmake_minimum_required(VERSION 3.25)
project(NeXo2 VERSION 0.1.0 LANGUAGES CXX)

# Estándar de C++ moderno
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Directorios de inclusión (donde estarán tus .h)
include_directories(include)

# Definir los módulos del emulador
add_subdirectory(src/common)
add_subdirectory(src/core)

# Ejecutable principal
add_executable(nexo2 src/main.cpp)

# Enlazar las partes
target_link_libraries(nexo2 core common)