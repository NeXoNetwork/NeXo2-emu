#!/usr/bin/env python3
"""Reaplica los parches locales de compatibilidad con MSVC al submodulo 'ballistic'.

Es IDEMPOTENTE: puedes ejecutarlo tantas veces como quieras. Ejecutalo despues
de un 'git submodule update' si los parches se hubieran perdido. El CMake del
proyecto tambien lo llama solo al configurar.

Parches:
  1) Deteccion de arquitectura para MSVC (_M_ARM64 / _M_X64).
  2) Shim de __FILE_NAME__ (MSVC no trae ese builtin).
  3) Quitar /WX (warnings-como-errores) del build de ballistic.
"""
import os

HERE     = os.path.dirname(os.path.abspath(__file__))
BAL      = os.path.normpath(os.path.join(HERE, "..", "externals", "ballistic"))
PLATFORM = os.path.join(BAL, "include", "bal_platform.h")
CMAKE    = os.path.join(BAL, "CMakeLists.txt")


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def _write(path, text):
    # newline="" -> no tocamos los fines de linea existentes.
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def patch_platform():
    if not os.path.exists(PLATFORM):
        print("  ! no encontrado:", PLATFORM)
        return
    s = original = _read(PLATFORM)
    nl = "\r\n" if "\r\n" in s else "\n"

    if "_M_ARM64" not in s:
        s = s.replace("#if defined(__aarch64__)",
                      "#if defined(__aarch64__) || defined(_M_ARM64)")
    if "_M_X64" not in s:
        s = s.replace("#elif defined(__x86_64__)",
                      "#elif defined(__x86_64__) || defined(_M_X64)")
    if "#define __FILE_NAME__ __FILE__" not in s:
        shim = ("/* MSVC no tiene el builtin __FILE_NAME__; usamos __FILE__. */" + nl +
                "#if defined(_MSC_VER) && !defined(__FILE_NAME__)" + nl +
                "#define __FILE_NAME__ __FILE__" + nl + "#endif" + nl + nl)
        s = s.replace("#endif /* BALLISTIC_PLATFORM_H */",
                      shim + "#endif /* BALLISTIC_PLATFORM_H */")

    if s != original:
        _write(PLATFORM, s)
        print("  + bal_platform.h parcheado")
    else:
        print("  = bal_platform.h ya estaba parcheado")


def patch_cmake():
    if not os.path.exists(CMAKE):
        print("  ! no encontrado:", CMAKE)
        return
    s = original = _read(CMAKE)
    if "/W4 /WX" in s:
        s = s.replace("add_compile_options(/W4 /WX)", "add_compile_options(/W4)")
    if s != original:
        _write(CMAKE, s)
        print("  + CMakeLists.txt de ballistic: quitado /WX")
    else:
        print("  = CMakeLists.txt de ballistic ya estaba parcheado")


def main():
    print("Reaplicando parches de compatibilidad MSVC a ballistic...")
    patch_platform()
    patch_cmake()
    print("Listo.")


if __name__ == "__main__":
    main()
