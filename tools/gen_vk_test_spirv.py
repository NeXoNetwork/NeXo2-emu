#!/usr/bin/env python3
"""
Compila los shaders GLSL de tests/shaders/vulkan a SPIR-V con glslangValidator y los
guarda como arrays de C++ en tests/generated/vk_test_spirv.hpp (solo para los tests de
Vulkan; los shaders de los juegos los traduce NeXo, sin glslang).

    python3 tools/gen_vk_test_spirv.py [--glslang ruta/a/glslangValidator]
"""
import argparse, os, pathlib, struct, subprocess, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "tests" / "shaders" / "vulkan"
OUT = ROOT / "tests" / "generated" / "vk_test_spirv.hpp"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glslang", default=os.environ.get("GLSLANG", "glslangValidator"))
    args = ap.parse_args()
    lines = ["// Generado por tools/gen_vk_test_spirv.py: no editar a mano.",
             "#pragma once", "#include <cstdint>", "", "namespace NeXo2::Tests {", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for f in sorted(SRC.iterdir()):
            if f.suffix not in (".comp", ".vert", ".frag"):
                continue
            out = os.path.join(tmp, "s.spv")
            subprocess.run([args.glslang, "-V", "--target-env", "vulkan1.1", "-o", out, str(f)], check=True,
                           stdout=subprocess.DEVNULL)
            data = open(out, "rb").read()
            words = struct.unpack(f"<{len(data)//4}I", data)
            name = f"k_spv_{f.stem}_{f.suffix[1:]}"
            lines.append(f"inline const uint32_t {name}[] = {{")
            for i in range(0, len(words), 8):
                lines.append("    " + " ".join(f"0x{w:08X}u," for w in words[i:i + 8]))
            lines.append("};")
            lines.append("")
    lines.append("} // namespace NeXo2::Tests")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"-> {OUT.relative_to(ROOT)}")

if __name__ == "__main__":
    main()
