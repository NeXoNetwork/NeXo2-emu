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
* `src/core/arm64/` (CPU interpreter, split by instruction group)
* `tests/` (CPU tests and the programs they run)
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

## 4. Run the CPU tests

The `nexo2_tests` target is built together with the emulator (disable it with
`-DNEXO2_BUILD_TESTS=OFF`). From the `build` folder:

```cmd
cmake --build . --config Release --target nexo2_tests
ctest -C Release --output-on-failure
```

Or run `Release\nexo2_tests.exe` directly to see every test.

## 5. Run a homebrew (.nro)

From the project root (so `assets/` and `tests/` are found):

```cmd
build\Release\NeXo2.exe tests\generated\hello.nro
```

Or start `NeXo2.exe` and drag a `.nro` file onto the window, or use the
"Cargar NRO" button. Only very simple homebrew runs for now: anything built
with libnx needs IPC services that are not emulated yet.

## 6. (Optional) Regenerate the test programs

Only needed if you change `tests/programs/`. Requires LLVM (`winget install LLVM.LLVM`):

```cmd
python tools/asm2cpp.py     :: tests/programs/*.S and *.c  -> tests/generated/test_programs.hpp
python tools/make_nro.py    :: tests/programs/nro_hello/   -> tests/generated/hello.nro
```
