# Build Guide - NeXo 2

Instructions to build the emulator using the MSVC v143 toolset.

## 1. Requirements
* Visual Studio 2022/2026 with the "Desktop development with C++" workload.
* MSVC v143 build tools installed.
* CMake 3.25 or higher.
* Python 3 (only for the optional tools in `tools/`).

## 2. File Structure
* `src/main.cpp`
* `src/core/memory/memory.hpp`
* `src/core/arm64/` (CPU interpreter, split by instruction group)
* `tests/` (CPU tests and the programs they run)
* `externals/` (SDL3, ImGui, dynarmic and ext-boost). After cloning run
  `git submodule update --init --recursive` (dynarmic has submodules of its own).
* `assets/` (logo.bmp, icon.ico)

> Use the **Release** build to run homebrew. Debug works, but the interpreter is many
> times slower there.

> **JIT**: `externals/dynarmic` (ARM64 -> x86-64 recompiler) and `externals/ext-boost`
> (Boost headers it needs; nothing of Boost is compiled) are built automatically. The first
> build takes a few minutes more. To build without them: `-DNEXO2_ENABLE_JIT=OFF`.

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
build\Release\NeXo2.exe path\to\some_homebrew.nro
```

Or start `NeXo2.exe` and drag a `.nro` file onto the window, or use the
"Cargar NRO" button. Then press **Run** (or **F5**): the program keeps running until
you press **Pausa** or it stops. What it draws appears in the **Pantalla** window.

Homebrew built with libnx runs (text console and software framebuffer). Commercial
games, GPU/3D, audio and multiple threads are not emulated yet. If a program needs
something missing, the CPU stops and the "Programa" window says exactly what, e.g.
`Servicio 'audren:u': comando 0 no implementado`.

### SD card

The emulated SD card (`sdmc:/`) is the `sdmc` folder **next to `NeXo2.exe`**
(e.g. `build\Release\sdmc`). The full path is shown in the "Programa" window. Put the
files a homebrew expects there; it cannot reach anything outside that folder.

### Controls

Click the **Pantalla** window first (while a text field has focus, keys go to the UI).

| Switch | Keyboard | Gamepad (by position) |
| :--- | :--- | :--- |
| D-pad | arrow keys | D-pad |
| A / B / X / Y | X / Z / S / A | right / bottom / top / left face button |
| L / R | Q / W | shoulders |
| ZL / ZR | 1 / 2 | triggers |
| + / − | Enter / Backspace | Start / Back |
| Left stick | T F G H | left stick |
| Right stick | I J K L | right stick |

The first gamepad connected to the PC is used automatically (its name appears in the
Pantalla window). Hover the **(?)** there to see this table. Hold buttons for a moment:
at the current speed a program only reads the controller a few times per second.

### Log

Everything the UI and the emulator print also goes to `nexo2.log` in the working folder.

## 6. (Optional) Regenerate the test programs

Only needed if you change `tests/programs/`. Requires LLVM (`winget install LLVM.LLVM`):

```cmd
python tools/asm2cpp.py     :: tests/programs/*.S and *.c  -> tests/generated/test_programs.hpp
python tools/make_nro.py    :: tests/programs/nro_*/       -> tests/generated/*.nro (hello, ipc, libnx_init)
```

The SIMD/FP reference values are generated on Linux/WSL with QEMU (`apt install qemu-user
gcc-aarch64-linux-gnu clang lld llvm`):

```bash
python3 tools/gen_simd_tests.py   # tests/programs/simd/*.S -> tests/generated/simd_tests.hpp
python3 tools/cpu_fuzz.py         # random instructions of every group -> tests/generated/cpu_fuzz.bin
```

See [docs/07-nexo-internals/cpu-fuzzing.md](docs/07-nexo-internals/cpu-fuzzing.md) for the options.

Third-party homebrew used for manual testing goes in `tests/homebrew/` (ignored by git).
