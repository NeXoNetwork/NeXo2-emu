#!/usr/bin/env python3
"""
Convierte los programas de prueba de tests/programs/ (.S en ensamblador ARM64 y
.c en C) en un header C++ con el codigo maquina listo para cargar en el emulador:
    tests/generated/test_programs.hpp

No hace falta ejecutarlo para compilar NeXo: el header generado ya esta en el repo.
Solo hay que ejecutarlo si cambias o anades un programa en tests/programs/.

Requisitos: clang, ld.lld y llvm-objdump (LLVM 15 o superior).
    En Windows: instala LLVM (winget install LLVM.LLVM) o usa WSL.

Uso (desde la raiz del proyecto):
    python tools/asm2cpp.py
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROGRAMS = ROOT / "tests" / "programs"
OUTPUT = ROOT / "tests" / "generated" / "test_programs.hpp"

TARGET = ["--target=aarch64-none-elf", "-march=armv8.2-a+lse"]
C_FLAGS = ["-O2", "-mgeneral-regs-only", "-ffreestanding", "-fno-builtin",
           "-fno-stack-protector", "-fno-pic", "-nostdlib", "-fno-asynchronous-unwind-tables"]

LINE_RE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$")
# Datos dentro del codigo (.word/.quad) salen como bytes sueltos: "68: ef cd ab 89  .word 0x..."
DATA_RE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ){3}[0-9a-f]{2})\s+(.*)$")
SYM_RE = re.compile(r"^([0-9a-f]+) <([A-Za-z_][A-Za-z0-9_]*)>:$")


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"Error ejecutando {' '.join(cmd)}:\n{result.stderr}")
    return result.stdout


def build(source: pathlib.Path, tmp: pathlib.Path):
    obj = tmp / (source.stem + ".o")
    elf = tmp / (source.stem + ".elf")
    flags = TARGET + (C_FLAGS if source.suffix == ".c" else [])
    run(["clang", *flags, "-c", str(source), "-o", str(obj)])
    # Enlazamos en la direccion 0: el codigo es relativo al PC y se puede cargar en cualquier sitio.
    run(["ld.lld", "-e", "0", "-Ttext=0", "--no-relax", str(obj), "-o", str(elf)])
    dump = run(["llvm-objdump", "-d", str(elf)])

    words, symbols = [], {}
    for line in dump.splitlines():
        sym = SYM_RE.match(line.strip())
        if sym:
            symbols[sym.group(2)] = int(sym.group(1), 16)
            continue
        m = LINE_RE.match(line)
        if m:
            asm = " ".join(m.group(3).split())
            words.append((int(m.group(1), 16), int(m.group(2), 16), asm))
            continue
        d = DATA_RE.match(line)
        if d:
            value = int.from_bytes(bytes.fromhex(d.group(2).replace(" ", "")), "little")
            words.append((int(d.group(1), 16), value, " ".join(d.group(3).split())))
    # El array se carga palabra a palabra: rellenamos huecos para que el indice = direccion / 4.
    filled = []
    for addr, word, asm in words:
        while len(filled) * 4 < addr:
            filled.append((len(filled) * 4, 0, "(relleno)"))
        filled.append((addr, word, asm))
    return filled, symbols


def main():
    for tool in ("clang", "ld.lld", "llvm-objdump"):
        if not shutil.which(tool):
            sys.exit(f"No se encuentra '{tool}'. Instala LLVM (ver cabecera de este script).")

    sources = sorted(PROGRAMS.glob("*.S")) + sorted(PROGRAMS.glob("*.c"))
    out = [
        "// GENERADO por tools/asm2cpp.py a partir de tests/programs/. NO EDITAR A MANO.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace NeXo2::Tests::Programs {",
        "",
    ]
    with tempfile.TemporaryDirectory() as tmp:
        for src in sources:
            words, symbols = build(src, pathlib.Path(tmp))
            name = src.stem
            out.append(f"// {src.name}")
            out.append(f"alignas(16) inline const uint32_t {name}[] = {{")
            for addr, word, asm in words:
                out.append(f"    0x{word:08X}u, // {addr:04X}: {asm}")
            out.append("};")
            if src.suffix == ".c":
                # Offsets de cada funcion dentro del binario
                for sym, addr in sorted(symbols.items(), key=lambda kv: kv[1]):
                    if not sym.startswith("$"):
                        out.append(f"inline constexpr uint64_t {name}_{sym} = 0x{addr:X};")
            out.append("")
    out.append("} // namespace NeXo2::Tests::Programs")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"Generado {OUTPUT.relative_to(ROOT)} ({len(sources)} programas)")


if __name__ == "__main__":
    main()
