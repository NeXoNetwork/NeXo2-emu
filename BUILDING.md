# Build Guide - NeXo 2

Instructions to build the emulator using the MSVC v143 toolset.

## 1. Requirements
* Visual Studio 2022/2026 with the "Desktop development with C++" workload.
* MSVC v143 build tools installed.
* CMake 3.25 or higher.
* Python 3 (required by the Ballistic submodule to generate its ARM64 decoder tables at build time).

## 2. File Structure
* `src/main.cpp`
* `src/core/memory/memory.hpp`
* `src/core/arm64/`
* `externals/` (SDL3, ImGui and Ballistic)
* `assets/` (logo.bmp, icon.ico)

## 3. Build Commands (CMD)

Run from the project root:

```cmd
if exist build rd /s /q build
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -T v143 -A x64
cmake --build . --config Release
```
