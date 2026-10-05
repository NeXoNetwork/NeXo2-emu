#!/usr/bin/env python3
"""
Genera los tests "diferenciales" de SIMD / coma flotante:
    tests/programs/simd/*.S  ->  tests/generated/simd_tests.hpp

Cada programa se ejecuta en un ARM64 de referencia (QEMU) y se guardan TODOS los
registros al final (x0-x28, NZCV, v0-v31). Los tests de NeXo ejecutan el mismo
codigo y comparan registro a registro. Asi no hay que calcular a mano los
resultados esperados: los da un procesador ARM "de verdad".

No hace falta ejecutarlo para compilar NeXo: el header generado ya esta en el repo.

Requisitos (Linux o WSL): clang, ld.lld, llvm-objdump, qemu-aarch64 (paquete qemu-user)
y aarch64-linux-gnu-gcc (paquete gcc-aarch64-linux-gnu).

Uso (desde la raiz del proyecto):
    python3 tools/gen_simd_tests.py
"""
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "tests" / "programs" / "simd"
OUTPUT = ROOT / "tests" / "generated" / "simd_tests.hpp"

# Reutilizamos el ensamblador/volcado de asm2cpp.py
spec = importlib.util.spec_from_file_location("asm2cpp", ROOT / "tools" / "asm2cpp.py")
asm2cpp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(asm2cpp)

# Envoltorio para QEMU: guarda los registros que el ABI de Linux exige conservar,
# pone todo a cero (como NeXo al arrancar), ejecuta la prueba y vuelca los registros.
WRAPPER = r"""
    .text
    .global run_snippet
run_snippet:
    stp  x29, x30, [sp, #-160]!
    stp  x19, x20, [sp, #16]
    stp  x21, x22, [sp, #32]
    stp  x23, x24, [sp, #48]
    stp  x25, x26, [sp, #64]
    stp  x27, x28, [sp, #80]
    stp  d8, d9,   [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    adrp x9, g_out
    add  x9, x9, :lo12:g_out
    str  x0, [x9]
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28
    mov  x\r, #0
    .endr
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    movi v\r\().2d, #0
    .endr
    msr  nzcv, xzr
    b    snippet_begin
snippet_end:
    stp  x0, x1, [sp, #-16]!
    adrp x0, g_out
    add  x0, x0, :lo12:g_out
    ldr  x0, [x0]
    stp  x2, x3, [x0, #16]
    stp  x4, x5, [x0, #32]
    stp  x6, x7, [x0, #48]
    stp  x8, x9, [x0, #64]
    stp  x10, x11, [x0, #80]
    stp  x12, x13, [x0, #96]
    stp  x14, x15, [x0, #112]
    stp  x16, x17, [x0, #128]
    stp  x18, x19, [x0, #144]
    stp  x20, x21, [x0, #160]
    stp  x22, x23, [x0, #176]
    stp  x24, x25, [x0, #192]
    stp  x26, x27, [x0, #208]
    str  x28, [x0, #224]
    ldp  x2, x3, [sp], #16
    stp  x2, x3, [x0]
    mrs  x2, nzcv
    str  x2, [x0, #232]
    add  x1, x0, #240
    stp  q0, q1, [x1], #32
    stp  q2, q3, [x1], #32
    stp  q4, q5, [x1], #32
    stp  q6, q7, [x1], #32
    stp  q8, q9, [x1], #32
    stp  q10, q11, [x1], #32
    stp  q12, q13, [x1], #32
    stp  q14, q15, [x1], #32
    stp  q16, q17, [x1], #32
    stp  q18, q19, [x1], #32
    stp  q20, q21, [x1], #32
    stp  q22, q23, [x1], #32
    stp  q24, q25, [x1], #32
    stp  q26, q27, [x1], #32
    stp  q28, q29, [x1], #32
    stp  q30, q31, [x1], #32
    ldp  x19, x20, [sp, #16]
    ldp  x21, x22, [sp, #32]
    ldp  x23, x24, [sp, #48]
    ldp  x25, x26, [sp, #64]
    ldp  x27, x28, [sp, #80]
    ldp  d8, d9,   [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    ldp  x29, x30, [sp], #160
    ret
    .bss
    .p2align 3
g_out: .quad 0
    .text
snippet_begin:
#include SNIPPET
"""

HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
extern void run_snippet(uint64_t* out);
static uint64_t out[29 + 1 + 64] __attribute__((aligned(16)));
int main(void) {
    run_snippet(out);
    for (int i = 0; i < 29 + 1 + 64; ++i) printf("%016llx\n", (unsigned long long)out[i]);
    return 0;
}
"""


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"Error ejecutando {' '.join(map(str, cmd))}:\n{r.stdout}{r.stderr}")
    return r.stdout


def reference_registers(snippet: pathlib.Path, tmp: pathlib.Path):
    (tmp / "wrapper.S").write_text(WRAPPER)
    (tmp / "harness.c").write_text(HARNESS)
    exe = tmp / f"{snippet.stem}.ref"
    run(["aarch64-linux-gnu-gcc", "-static", "-O0", "-march=armv8.2-a", "-DREFERENCE",
         f"-DSNIPPET=\"{snippet.name}\"", f"-I{SRC}", str(tmp / "harness.c"), str(tmp / "wrapper.S"),
         "-o", str(exe)])
    values = [int(line, 16) for line in run(["qemu-aarch64", str(exe)]).split()]
    return values[:29], values[29], values[30:]


def main():
    for tool in ("clang", "ld.lld", "llvm-objdump", "qemu-aarch64", "aarch64-linux-gnu-gcc"):
        if not shutil.which(tool):
            sys.exit(f"No se encuentra '{tool}' (ver cabecera de este script).")

    out = [
        "// GENERADO por tools/gen_simd_tests.py. NO EDITAR A MANO.",
        "// Registros esperados obtenidos ejecutando cada programa en QEMU (ARM64 de referencia).",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace NeXo2::Tests::Simd {",
        "",
        "struct Case {",
        "    const char* name;",
        "    const uint32_t* code;",
        "    size_t code_words;",
        "    uint64_t x[29];      // x0..x28",
        "    uint64_t nzcv;",
        "    uint64_t v[64];      // v0.lo, v0.hi, v1.lo, ...",
        "};",
        "",
    ]
    names = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        for snippet in sorted(SRC.glob("*.S")):
            words, _ = asm2cpp.build_preprocessed(snippet, tmp, [f"-I{SRC}"])
            x, nzcv, v = reference_registers(snippet, tmp)
            name = snippet.stem
            names.append(name)
            out.append(f"// {snippet.name}")
            out.append(f"alignas(16) inline const uint32_t code_{name}[] = {{")
            for addr, word, asm in words:
                out.append(f"    0x{word:08X}u, // {addr:04X}: {asm}")
            out.append("};")
            out.append(f"inline const Case case_{name} = {{")
            out.append(f'    "{name}", code_{name}, sizeof(code_{name}) / 4,')
            out.append("    {" + ", ".join(f"0x{val:X}ULL" for val in x) + "},")
            out.append(f"    0x{nzcv:X}ULL,")
            out.append("    {" + ", ".join(f"0x{val:X}ULL" for val in v) + "},")
            out.append("};")
            out.append("")
            print(f"  {snippet.name}: {len(words)} instrucciones")
    out.append("inline const Case* const kCases[] = {")
    out.extend(f"    &case_{n}," for n in names)
    out.append("};")
    out.append("")
    out.append("} // namespace NeXo2::Tests::Simd")
    OUTPUT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"Generado {OUTPUT.relative_to(ROOT)} ({len(names)} programas)")


if __name__ == "__main__":
    main()
