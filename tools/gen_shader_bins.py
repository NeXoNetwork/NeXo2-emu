#!/usr/bin/env python3
"""
Compila los shaders de prueba (tests/shaders/*.vert, *.frag) con uam y los guarda en
tests/generated/shader_bins.hpp para los tests de la GPU.

    python3 tools/gen_shader_bins.py --uam ruta/a/uam

No hace falta para compilar NeXo: el .hpp generado ya esta en el repo.
"""
import argparse, os, pathlib, struct, subprocess, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "tests" / "shaders"
OUT = ROOT / "tests" / "generated" / "shader_bins.hpp"
# El homebrew de prueba del triangulo lleva sus shaders dentro (en C)
OUT_C = ROOT / "tests" / "programs" / "nro_triangle" / "shaders.h"
C_SHADERS = ("tri_vert", "color_frag")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uam", default=os.environ.get("UAM", "uam"))
    args = ap.parse_args()
    lines = ["// Generado por tools/gen_shader_bins.py: no editar a mano.",
             "// Shaders de tests/shaders compilados con uam (codigo Maxwell de verdad).",
             "#pragma once", "#include <cstdint>", "", "namespace NeXo2::Tests {", "",
             "struct ShaderBin {",
             "    const uint8_t* code;   // seccion de codigo del .dksh (relleno + SPH + instrucciones)",
             "    uint32_t size;",
             "    uint32_t entry;        // offset de la SPH: lo que se pone en SetProgram.Offset",
             "    uint32_t cb1_offset;   // constantes del compilador (c[1]), dentro de 'code'",
             "    uint32_t cb1_size;",
             "};", ""]
    clines = ["// Generado por tools/gen_shader_bins.py: no editar a mano.",
              "// Shaders de tests/shaders compilados con uam, para el homebrew del triangulo.",
              "#pragma once", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for f in sorted(SRC.iterdir()):
            stage = {".vert": "vert", ".frag": "frag"}.get(f.suffix)
            if not stage:
                continue
            out = os.path.join(tmp, "s.dksh")
            subprocess.run([args.uam, "-s", stage, "-o", out, str(f)], check=True)
            d = open(out, "rb").read()
            h = struct.unpack_from("<6I", d, 0)
            ph = struct.unpack_from("<16I", d, h[4])
            code = d[h[2]:]
            name = f.stem + "_" + stage
            lines.append(f"inline const uint8_t k_{name}_code[] = {{")
            for i in range(0, len(code), 24):
                lines.append("    " + " ".join(f"0x{b:02X}," for b in code[i:i + 24]))
            lines.append("};")
            lines.append(f"inline const ShaderBin k_{name}{{k_{name}_code, {len(code)}, 0x{ph[1]:X}, 0x{ph[3]:X}, {ph[4]}}};")
            lines.append("")
            if name in C_SHADERS:
                up = name.upper()
                clines.append(f"#define {up}_SIZE {len(code)}u")
                clines.append(f"#define {up}_ENTRY 0x{ph[1]:X}u")
                clines.append(f"static const u8 {name}_code[{len(code)}] __attribute__((aligned(256))) = {{")
                for i in range(0, len(code), 16):
                    clines.append("    " + " ".join(f"0x{b:02X}," for b in code[i:i + 16]))
                clines.append("};")
                clines.append("")
    lines.append("} // namespace NeXo2::Tests")
    OUT_C.parent.mkdir(parents=True, exist_ok=True)
    OUT_C.write_text("\n".join(clines) + "\n")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"-> {OUT.relative_to(ROOT)}")

if __name__ == "__main__":
    main()
