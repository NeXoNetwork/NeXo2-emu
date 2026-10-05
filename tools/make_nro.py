#!/usr/bin/env python3
"""
Compila el homebrew de prueba (tests/programs/nro_hello/) y lo empaqueta como NRO:
    tests/generated/hello.nro

No hace falta ejecutarlo para compilar NeXo: el .nro generado ya esta en el repo.
Solo hay que ejecutarlo si cambias tests/programs/nro_hello/.

Requisitos: clang, ld.lld, llvm-objcopy y llvm-nm (LLVM 15 o superior).
    En Windows: winget install LLVM.LLVM

Uso (desde la raiz del proyecto):
    python tools/make_nro.py

Como se construye un NRO (ver docs/04-formats/nro.md):
  1. Se compila crt0.S + main.c para ARM64 y se enlaza desde la direccion 0
     con link.ld: .text, .rodata y .data alineados a 4 KB.
  2. Se vuelca la imagen tal cual (llvm-objcopy -O binary).
  3. Se escribe la cabecera NRO0 en el offset 0x10 con los tamanos de cada segmento.
  4. Se anade un bloque "ASET" con un NACP (nombre y autor de la aplicacion).
"""
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "tests" / "programs" / "nro_hello"
OUTPUT = ROOT / "tests" / "generated" / "hello.nro"

TITLE = "NeXo Hello"
AUTHOR = "NeXo 2 tests"

CFLAGS = ["--target=aarch64-none-elf", "-march=armv8.2-a", "-O2", "-mgeneral-regs-only",
          "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-nostdlib",
          "-fno-asynchronous-unwind-tables", "-Wall"]
PAGE = 0x1000


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"Error ejecutando {' '.join(map(str, cmd))}:\n{r.stdout}{r.stderr}")
    return r.stdout


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def main():
    for tool in ("clang", "ld.lld", "llvm-objcopy", "llvm-nm", "llvm-readelf"):
        if not shutil.which(tool):
            sys.exit(f"No se encuentra '{tool}'. Instala LLVM (ver cabecera de este script).")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        objs = []
        for src in (SRC / "crt0.S", SRC / "main.c"):
            obj = tmp / (src.name + ".o")
            run(["clang", *CFLAGS, "-c", str(src), "-o", str(obj)])
            objs.append(obj)
            # El NRO se carga en cualquier direccion: no puede haber direcciones absolutas.
            relocs = run(["llvm-readelf", "-r", str(obj)])
            if "R_AARCH64_ABS64" in relocs:
                sys.exit(f"{src.name} usa direcciones absolutas (R_AARCH64_ABS64). "
                         "Evita tablas de punteros globales en el codigo de prueba.")

        elf = tmp / "hello.elf"
        run(["ld.lld", "-T", str(SRC / "link.ld"), "--no-relax", *map(str, objs), "-o", str(elf)])

        # Direcciones de las secciones y simbolos
        sections = {}
        for line in run(["llvm-readelf", "-S", "-W", str(elf)]).splitlines():
            parts = line.replace("[", " ").replace("]", " ").split()
            if len(parts) > 6 and parts[1] in (".text", ".rodata", ".data", ".bss"):
                sections[parts[1]] = (int(parts[3], 16), int(parts[5], 16))  # (addr, size)
        symbols = {}
        for line in run(["llvm-nm", str(elf)]).splitlines():
            parts = line.split()
            if len(parts) == 3:
                symbols[parts[2]] = int(parts[0], 16)

        text_off, text_size = sections[".text"]
        ro_off, ro_size = sections.get(".rodata", (align(text_off + text_size, PAGE), 0))
        data_off = sections.get(".data", (align(ro_off + ro_size, PAGE), 0))[0]
        if ro_size == 0:
            ro_off = align(text_off + text_size, PAGE)
        bss_start, bss_end = symbols["__bss_start"], symbols["__bss_end"]
        data_size = bss_start - data_off
        bss_size = bss_end - bss_start
        image_size = data_off + data_size
        assert text_off == 0 and ro_off % PAGE == 0 and data_off % PAGE == 0

        raw = tmp / "hello.bin"
        run(["llvm-objcopy", "-O", "binary", "--remove-section=.bss", str(elf), str(raw)])
        image = bytearray(raw.read_bytes())
        image = image[:image_size] + bytes(max(0, image_size - len(image)))

    # Cabecera NRO0 en 0x10 (0x70 bytes). Ver src/core/loader/nro.hpp
    header = struct.pack("<4sIII" "II" "II" "II" "II" "32s" "II" "II" "II" "II",
                         b"NRO0", 0, image_size, 0,
                         text_off, text_size, ro_off, ro_size,
                         data_off, data_size, bss_size, 0,
                         b"NeXo2-test-hello".ljust(32, b"\0"),
                         0, 0, 0, 0, 0, 0, 0, 0)
    assert len(header) == 0x70
    image[0x10:0x80] = header

    # Bloque de assets: AssetHeader (0x38) + NACP (0x4000) con nombre y autor en todos los idiomas
    nacp = bytearray(0x4000)
    for lang in range(16):
        base = lang * 0x300
        nacp[base:base + len(TITLE)] = TITLE.encode()
        nacp[base + 0x200:base + 0x200 + len(AUTHOR)] = AUTHOR.encode()
    nacp[0x3060:0x3065] = b"1.0.0"  # DisplayVersion
    aset = struct.pack("<4sI" "QQ" "QQ" "QQ", b"ASET", 0, 0, 0, 0x38, len(nacp), 0, 0)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(bytes(image) + aset + bytes(nacp))
    print(f"Generado {OUTPUT.relative_to(ROOT)}: imagen {image_size:#x} bytes "
          f"(.text {text_size:#x}, .rodata {ro_size:#x}, .data {data_size:#x}, .bss {bss_size:#x})")


if __name__ == "__main__":
    main()
