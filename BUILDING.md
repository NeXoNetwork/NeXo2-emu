# Guía de Compilación - NeXo 2

Instrucciones para compilar el emulador utilizando el conjunto de herramientas MSVC v143.

## 1. Requisitos
* Visual Studio 2022/2026 con C++ Desktop Development.
* Herramientas de compilación MSVC v143 instaladas.
* CMake 3.25 o superior.

## 2. Estructura de archivos
* `src/main.cpp`
* `src/core/memory/memory.hpp`
* `src/core/arm64/`
* `externals/` (SDL3 e ImGui)
* `assets/` (logo.bmp, icon.ico)

## 3. Comandos de Compilación (CMD)

Ejecutar en la raíz del proyecto:

```cmd
if exist build rd /s /q build
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -T v143 -A x64
cmake --build . --config Release

