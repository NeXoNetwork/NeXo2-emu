#!/usr/bin/env python3
"""
Pruebas del interprete de shaders de la GPU con programas al azar.

    python3 tools/shader_fuzz.py --uam ruta/a/uam              # genera tests/generated/shader_fuzz.bin
    python3 tools/shader_fuzz.py --uam ruta/a/uam --count 400  # mas programas
    python3 tools/shader_fuzz.py --uam ruta/a/uam --show 12    # ensena el GLSL del programa 12

Para cada prueba:
  1. Se inventan unos valores de entrada (un "uniform buffer" con 4 vec4 y 4 ivec4).
  2. Se escribe un shader de vertices GLSL con 16 expresiones al azar (floats, ints y
     uints: sumas, multiplicaciones, divisiones, bits, comparaciones, bucles, if/else...).
  3. Se compila con uam (el compilador de shaders de deko3d) a codigo Maxwell de verdad.
  4. Se calcula aqui, en Python, el resultado esperado de cada expresion.
tests/gpu_shader_tests.cpp ejecuta el codigo Maxwell en el interprete de NeXo y compara.

No hace falta para compilar NeXo: el .bin generado ya esta en el repo.
uam: https://github.com/devkitPro/uam (se compila con meson; ver docs/07-nexo-internals/gpu-shaders.md).

Formato del .bin (little endian):
  "NXSF" u32 version u32 n_pruebas
  por prueba: u32 len_glsl, glsl (relleno a 4), u32 len_dksh, dksh (relleno a 4),
              u32 ubo[32], 16 x { u32 tipo (0 float, 1 entero), u32 esperado,
                                  u32 esperado_alt, f32 tolerancia }
"""
import argparse
import math
import os
import pathlib
import random
import re
import struct
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "generated" / "shader_fuzz.bin"

F32 = np.float32
np.seterr(all="ignore")


def f32(x):
    return float(F32(x))


def u32(x):
    return int(x) & 0xFFFFFFFF


def s32(x):
    x = u32(x)
    return x - (1 << 32) if x & 0x80000000 else x


def fbits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


class Gen:
    """Genera expresiones GLSL y a la vez calcula su valor."""

    def __init__(self, rng):
        self.r = rng
        # Uniform buffer: f[4] (vec4) e i[4] (ivec4)
        self.f = [round(rng.uniform(-10, 10), 3) for _ in range(16)]
        self.f = [f32(v) for v in self.f]
        self.i = []
        for _ in range(16):
            k = rng.random()
            if k < 0.5:
                self.i.append(rng.randint(-1000, 1000))
            elif k < 0.8:
                self.i.append(s32(rng.getrandbits(32)))
            else:
                self.i.append(rng.choice([0, 1, -1, 2, 7, 31, 32, 0x7FFFFFFF, -0x80000000, 0xFFFF, 0x10000]))
        self.ileaves = []   # (codigo, valor) de variables int creadas por los bucles e ifs
        self.fleaves = []   # (codigo, v32, v64, magnitud)
        self.body = []
        self.tmp = 0
        self.force_f = None   # --op: usar solo esta operacion (para encontrar fallos)
        self.force_i = None

    # ---------------- hojas ----------------
    def fleaf(self):
        r = self.r
        if self.fleaves and r.random() < 0.3:
            return r.choice(self.fleaves)
        if r.random() < 0.8:
            k = r.randrange(16)
            v = self.f[k]
            return (f"u.f[{k // 4}].{'xyzw'[k % 4]}", v, v, abs(v))
        v = f32(round(r.uniform(-4, 4), 2))
        return (f"({v!r})", v, v, abs(v))

    def ileaf(self):
        r = self.r
        if self.ileaves and r.random() < 0.3:
            return r.choice(self.ileaves)
        if r.random() < 0.85:
            k = r.randrange(16)
            return (f"u.i[{k // 4}].{'xyzw'[k % 4]}", self.i[k])
        v = r.randint(-50, 50)
        return (f"({v})", v)

    # ---------------- floats ----------------
    def fexpr(self, d):
        r = self.r
        if d <= 0 or r.random() < 0.2:
            return self.fleaf()
        op = self.force_f if self.force_f and d >= 1 else None
        op = op or r.choice(["add", "sub", "mul", "div", "min", "max", "neg", "abs", "floor", "ceil", "fract",
                       "clamp", "mix", "sqrt", "isqrt", "exp2", "log2", "sin", "cos", "i2f", "u2f",
                       "sel", "fma", "sign", "step", "pow"])
        if op in ("add", "sub", "mul", "min", "max", "fma", "mix"):
            a = self.fexpr(d - 1)
            b = self.fexpr(d - 1)
            ca, a32, a64, ma = a
            cb, b32, b64, mb = b
            if op == "add":
                return (f"({ca} + {cb})", f32(F32(a32) + F32(b32)), a64 + b64, max(ma, mb, abs(a64 + b64)))
            if op == "sub":
                return (f"({ca} - {cb})", f32(F32(a32) - F32(b32)), a64 - b64, max(ma, mb, abs(a64 - b64)))
            if op == "mul":
                v = a64 * b64
                return (f"({ca} * {cb})", f32(F32(a32) * F32(b32)), v, max(ma, mb, abs(v), ma * mb))
            if op == "min":
                return (f"min({ca}, {cb})", min(a32, b32), min(a64, b64), max(ma, mb))
            if op == "max":
                return (f"max({ca}, {cb})", max(a32, b32), max(a64, b64), max(ma, mb))
            if op == "mix":
                v32 = f32(F32(a32) * F32(0.75) + F32(b32) * F32(0.25))
                v64 = a64 * 0.75 + b64 * 0.25
                return (f"mix({ca}, {cb}, 0.25)", v32, v64, max(ma, mb))
            c = self.fexpr(d - 1)
            cc, c32, c64, mc = c
            v = a64 * b64 + c64
            return (f"fma({ca}, {cb}, {cc})", f32(F32(a32) * F32(b32) + F32(c32)), v,
                    max(ma, mb, mc, ma * mb, abs(v)))
        if op == "div":
            a = self.fexpr(d - 1)
            b = self.fexpr(d - 1)
            ca, a32, a64, ma = a
            cb, b32, b64, mb = b
            den32 = f32(abs(F32(b32)) + F32(0.5))
            den64 = abs(b64) + 0.5
            v = a64 / den64
            return (f"({ca} / (abs({cb}) + 0.5))", f32(F32(a32) / F32(den32)), v, max(ma, abs(v), ma * 2) * (1 + mb))
        if op in ("neg", "abs"):
            ca, a32, a64, ma = self.fexpr(d - 1)
            if op == "neg":
                return (f"(-{ca})", -a32, -a64, ma)
            return (f"abs({ca})", abs(a32), abs(a64), ma)
        if op in ("floor", "ceil", "fract", "sign"):
            # Solo sobre hojas: un error de 1 ulp en un valor calculado podria saltar de entero
            ca, a32, a64, ma = self.fleaf()
            fn = {"floor": math.floor, "ceil": math.ceil}.get(op)
            if fn:
                v = float(fn(a32))
            elif op == "fract":
                v = f32(F32(a32) - F32(math.floor(a32)))
            else:
                v = 1.0 if a32 > 0 else -1.0 if a32 < 0 else 0.0
            return (f"{op}({ca})", v, v, max(ma, 1))
        if op == "step":
            a = self.fleaf()
            b = self.fleaf()
            v = 0.0 if b[1] < a[1] else 1.0
            return (f"step({a[0]}, {b[0]})", v, v, 1)
        if op == "clamp":
            ca, a32, a64, ma = self.fexpr(d - 1)
            return (f"clamp({ca}, -1.0, 1.0)", min(max(a32, -1.0), 1.0), min(max(a64, -1.0), 1.0), max(ma, 1))
        if op in ("sqrt", "isqrt", "log2"):
            ca, a32, a64, ma = self.fexpr(d - 1)
            x32 = abs(a32) + (1.0 if op != "sqrt" else 0.0)
            x64 = abs(a64) + (1.0 if op != "sqrt" else 0.0)
            arg = f"abs({ca})" + (" + 1.0" if op != "sqrt" else "")
            if op == "sqrt":
                return (f"sqrt({arg})", f32(np.sqrt(F32(x32))), math.sqrt(x64), max(ma, 1) * 2)
            if op == "isqrt":
                return (f"inversesqrt({arg})", f32(F32(1) / np.sqrt(F32(x32))), 1 / math.sqrt(x64), max(ma, 1))
            return (f"log2({arg})", f32(np.log2(F32(x32))), math.log2(x64), max(ma, 1) * 2)
        if op == "exp2":
            ca, a32, a64, ma = self.fexpr(d - 1)
            x32 = min(max(a32, -8.0), 8.0)
            x64 = min(max(a64, -8.0), 8.0)
            v = 2.0 ** x64
            return (f"exp2(clamp({ca}, -8.0, 8.0))", f32(np.exp2(F32(x32))), v, max(ma, v, 1) * 8)
        if op == "pow":
            ca, a32, a64, ma = self.fexpr(d - 1)
            e = r.choice([0.5, 2.0, 1.0 / 2.2, 3.0])
            x32 = abs(a32) + 0.25
            x64 = abs(a64) + 0.25
            v = x64 ** e
            return (f"pow(abs({ca}) + 0.25, {e!r})", f32(np.exp2(F32(e) * np.log2(F32(x32)))), v, max(ma, v, 1) * 8)
        if op in ("sin", "cos"):
            ca, a32, a64, ma = self.fexpr(d - 1)
            fn = np.sin if op == "sin" else np.cos
            return (f"{op}({ca})", f32(fn(F32(a32))), (math.sin if op == "sin" else math.cos)(a64), max(ma, 1))
        if op == "i2f":
            ci, vi = self.iexpr(d - 1)
            v = f32(float(s32(vi)))
            return (f"float({ci})", v, v, abs(v))
        if op == "u2f":
            ci, vi = self.iexpr(d - 1)
            v = f32(float(u32(vi)))
            return (f"float(uint({ci}))", v, v, abs(v))
        if op == "sel":
            a = self.fleaf()
            b = self.fleaf()
            t = self.fexpr(d - 1)
            e = self.fexpr(d - 1)
            cmp = r.choice(["<", ">", "<=", ">=", "==", "!="])
            res = {"<": a[1] < b[1], ">": a[1] > b[1], "<=": a[1] <= b[1], ">=": a[1] >= b[1],
                   "==": a[1] == b[1], "!=": a[1] != b[1]}[cmp]
            pick = t if res else e
            return (f"(({a[0]} {cmp} {b[0]}) ? {t[0]} : {e[0]})", pick[1], pick[2], max(t[3], e[3]))
        raise AssertionError(op)

    # ---------------- enteros ----------------
    def iexpr(self, d):
        r = self.r
        if d <= 0 or r.random() < 0.2:
            return self.ileaf()
        op = self.force_i if self.force_i and d >= 1 else None
        op = op or r.choice(["add", "sub", "mul", "div", "mod", "and", "or", "xor", "not", "shl", "shr", "ushr",
                       "min", "max", "umin", "umax", "abs", "neg", "bfe", "ubfe", "bfi", "popc", "msb",
                       "lsb", "f2i", "f2u", "sel", "usel", "udiv", "umod"])
        if op in ("f2i", "f2u"):
            # Solo valores del uniform: un float calculado podria quedar a 1 ulp de un entero
            k = r.randrange(16)
            v32 = self.f[k]
            c = f"u.f[{k // 4}].{'xyzw'[k % 4]}"
            if op == "f2i":
                return (f"int({c})", int(math.trunc(v32)))
            return (f"int(uint(abs({c})))", int(math.trunc(abs(v32))))
        if op in ("abs",):
            c, v = self.ileaf()
            return (f"abs({c})", s32(abs(s32(v))))
        if op in ("not", "neg", "popc", "msb", "lsb"):
            c, v = self.iexpr(d - 1)
            v = u32(v)
            if op == "not":
                return (f"(~{c})", s32(~v))
            if op == "neg":
                return (f"(-{c})", s32(-s32(v)))
            if op == "popc":
                return (f"bitCount({c})", bin(v).count("1"))
            if op == "lsb":
                return (f"findLSB({c})", (v & -v).bit_length() - 1 if v else -1)
            sv = s32(v)
            x = v if sv >= 0 else u32(~v)
            return (f"findMSB({c})", x.bit_length() - 1 if x else -1)
        if op in ("bfe", "ubfe"):
            c, v = self.iexpr(d - 1)
            off = r.randint(0, 31)
            bits = r.randint(1, 32 - off)
            x = (u32(v) >> off) & ((1 << bits) - 1)
            if op == "bfe":
                if bits < 32 and (x >> (bits - 1)) & 1:
                    x -= 1 << bits
                return (f"bitfieldExtract({c}, {off}, {bits})", s32(x))
            return (f"int(bitfieldExtract(uint({c}), {off}, {bits}))", s32(x))
        if op == "bfi":
            a = self.iexpr(d - 1)
            b = self.iexpr(d - 1)
            off = r.randint(0, 31)
            bits = r.randint(1, 32 - off)
            mask = ((1 << bits) - 1) << off
            v = (u32(a[1]) & ~mask) | ((u32(b[1]) << off) & mask)
            return (f"bitfieldInsert({a[0]}, {b[0]}, {off}, {bits})", s32(v))
        if op in ("sel", "usel"):
            a = self.iexpr(d - 1)
            b = self.iexpr(d - 1)
            t = self.iexpr(d - 1)
            e = self.iexpr(d - 1)
            cmp = r.choice(["<", ">", "<=", ">=", "==", "!="])
            if op == "sel":
                x, y = s32(a[1]), s32(b[1])
                ca, cb = a[0], b[0]
            else:
                x, y = u32(a[1]), u32(b[1])
                ca, cb = f"uint({a[0]})", f"uint({b[0]})"
            res = {"<": x < y, ">": x > y, "<=": x <= y, ">=": x >= y, "==": x == y, "!=": x != y}[cmp]
            return (f"(({ca} {cmp} {cb}) ? {t[0]} : {e[0]})", t[1] if res else e[1])
        a = self.iexpr(d - 1)
        b = self.iexpr(d - 1)
        if op == "mul":
            # La version de mesa que lleva uam calcula mal x * (constante negativa) en
            # algunos casos (x * -7 le sale x * -9, x * -2 le sale 0...): si un operando es
            # constante y negativo, lo cambiamos de signo.
            def fix(e):
                if not re.search(r"u\.|\bt\d", e[0]) and s32(e[1]) < 0:
                    return (f"(-{e[0]})", s32(-s32(e[1])))
                return e
            a, b = fix(a), fix(b)
        ca, va = a
        cb, vb = b
        if op == "add":
            return (f"({ca} + {cb})", s32(va + vb))
        if op == "sub":
            return (f"({ca} - {cb})", s32(va - vb))
        if op == "mul":
            return (f"({ca} * {cb})", s32(va * vb))
        # Divisiones solo entre constantes: con un divisor variable uam no sabe hacerlo
        # exacto (lo emula con floats y avisa); con constante usa multiplicaciones (XMAD/IMAD.HI).
        if op in ("div", "mod", "udiv", "umod"):
            den = r.choice([1, 2, 3, 5, 7, 10, 16, 100, 255, 1000, 4096, 65535, 123457])
            if op == "div":
                q = abs(s32(va)) // den * (1 if s32(va) >= 0 else -1)
                return (f"({ca} / {den})", s32(q))
            if op == "mod":
                return (f"(({ca} & 0x7FFFFFFF) % {den})", (u32(va) & 0x7FFFFFFF) % den)
            if op == "udiv":
                return (f"int(uint({ca}) / {den}u)", s32(u32(va) // den))
            return (f"int(uint({ca}) % {den}u)", s32(u32(va) % den))
        if op == "and":
            return (f"({ca} & {cb})", s32(u32(va) & u32(vb)))
        if op == "or":
            return (f"({ca} | {cb})", s32(u32(va) | u32(vb)))
        if op == "xor":
            return (f"({ca} ^ {cb})", s32(u32(va) ^ u32(vb)))
        if op == "shl":
            return (f"({ca} << ({cb} & 31))", s32(u32(va) << (u32(vb) & 31)))
        if op == "shr":
            return (f"({ca} >> ({cb} & 31))", s32(s32(va) >> (u32(vb) & 31)))
        if op == "ushr":
            return (f"int(uint({ca}) >> (uint({cb}) & 31u))", s32(u32(va) >> (u32(vb) & 31)))
        if op == "min":
            return (f"min({ca}, {cb})", min(s32(va), s32(vb)))
        if op == "max":
            return (f"max({ca}, {cb})", max(s32(va), s32(vb)))
        if op == "umin":
            return (f"int(min(uint({ca}), uint({cb})))", s32(min(u32(va), u32(vb))))
        if op == "umax":
            return (f"int(max(uint({ca}), uint({cb})))", s32(max(u32(va), u32(vb))))
        raise AssertionError(op)

    # ---------------- sentencias (crean variables nuevas) ----------------
    def statement(self):
        r = self.r
        n = self.tmp
        self.tmp += 1
        kind = r.choice(["iif", "fif", "iloop", "floop", "brk"])
        if kind == "iif":
            a = self.iexpr(2); b = self.iexpr(2); t = self.iexpr(2); e = self.iexpr(2)
            cmp = r.choice(["<", ">", "==", "!="])
            res = {"<": s32(a[1]) < s32(b[1]), ">": s32(a[1]) > s32(b[1]),
                   "==": u32(a[1]) == u32(b[1]), "!=": u32(a[1]) != u32(b[1])}[cmp]
            self.body.append(f"    int t{n};\n    if ({a[0]} {cmp} {b[0]}) t{n} = {t[0]};\n    else t{n} = {e[0]};")
            self.ileaves.append((f"t{n}", t[1] if res else e[1]))
        elif kind == "fif":
            a = self.fleaf(); b = self.fleaf(); t = self.fexpr(2); e = self.fexpr(2)
            res = a[1] < b[1]
            self.body.append(f"    float t{n} = {e[0]};\n    if ({a[0]} < {b[0]}) t{n} = {t[0]};")
            p = t if res else e
            self.fleaves.append((f"t{n}", p[1], p[2], p[3]))
        elif kind == "iloop":
            init = self.iexpr(2)
            k = r.randrange(16)
            count = self.i[k] & 7
            mul = r.choice([3, 5, 7, 31])
            add = self.ileaf()
            v = init[1]
            for j in range(count):
                v = s32(s32(v) * mul + add[1] + j)
            self.body.append(f"    int t{n} = {init[0]};\n    for (int k = 0; k < (u.i[{k // 4}].{'xyzw'[k % 4]} & 7); k++)\n"
                             f"        t{n} = t{n} * {mul} + {add[0]} + k;")
            self.ileaves.append((f"t{n}", v))
        elif kind == "floop":
            init = self.fexpr(1)
            k = r.randrange(16)
            count = self.i[k] & 7
            add = self.fleaf()
            v32, v64, m = init[1], init[2], max(init[3], add[3])
            for j in range(count):
                v32 = f32(F32(v32) * F32(0.5) + F32(add[1]))
                v64 = v64 * 0.5 + add[2]
                m = max(m, abs(v64))
            self.body.append(f"    float t{n} = {init[0]};\n    for (int k = 0; k < (u.i[{k // 4}].{'xyzw'[k % 4]} & 7); k++)\n"
                             f"        t{n} = t{n} * 0.5 + {add[0]};")
            self.fleaves.append((f"t{n}", v32, v64, m * 2))
        else:   # bucle con break
            k = r.randrange(16)
            limit = r.randint(0, 9)
            stop = self.ileaf()
            stop_at = u32(stop[1]) & 7
            v = 0
            for j in range(limit):
                if j == stop_at:
                    break
                v = v + j * 2 + 1
            self.body.append(f"    int t{n} = 0;\n    for (int k = 0; k < {limit}; k++) {{\n"
                             f"        if (k == ({stop[0]} & 7)) break;\n        t{n} += k * 2 + 1;\n    }}")
            self.ileaves.append((f"t{n}", v))


HEADER = """#version 460
layout (std140, binding = 0) uniform U { vec4 f[4]; ivec4 i[4]; } u;
layout (location = 0) out vec4 of0;
layout (location = 1) out vec4 of1;
layout (location = 2) flat out ivec4 oi0;
layout (location = 3) flat out ivec4 oi1;
void main() {
"""


def make_case(seed, op=None):
    g = Gen(random.Random(seed))
    if op:
        g.force_f = op[2:] if op.startswith("f:") else None
        g.force_i = op[2:] if op.startswith("i:") else None
    for _ in range(0 if op else g.r.randint(0, 3)):
        g.statement()
    outs = []
    lines = []
    for loc, name in ((0, "of0"), (1, "of1")):
        comps = []
        for c in "xyzw":
            e = g.fexpr(1 if op else g.r.randint(1, 4))
            comps.append(e)
            lines.append(f"    {name}.{c} = {e[0]};")
        for e in comps:
            tol = 2e-5 * max(e[3], 1.0) + 1e-6
            outs.append((0, fbits(e[1]), fbits(f32(e[2])), tol))
    for name in ("oi0", "oi1"):
        for c in "xyzw":
            e = g.iexpr(1 if op else g.r.randint(1, 4))
            lines.append(f"    {name}.{c} = {e[0]};")
            outs.append((1, u32(e[1]), u32(e[1]), 0.0))
    glsl = HEADER + "\n".join(g.body) + ("\n" if g.body else "") + "\n".join(lines) + "\n    gl_Position = vec4(0.0);\n}\n"
    ubo = [fbits(v) for v in g.f] + [u32(v) for v in g.i]
    return glsl, ubo, outs


def compile_glsl(uam, glsl, tmp):
    src = os.path.join(tmp, "s.vert")
    out = os.path.join(tmp, "s.dksh")
    with open(src, "w") as f:
        f.write(glsl)
    p = subprocess.run([uam, "-s", "vert", "-o", out, src], capture_output=True, text=True)
    if p.returncode != 0:
        return None, p.stderr + p.stdout
    return open(out, "rb").read(), ""


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uam", default=os.environ.get("UAM", "uam"))
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=None)
    ap.add_argument("--op", default=None, help="solo esta operacion, p. ej. i:xor o f:div (para depurar)")
    ap.add_argument("--out", default=str(OUT))
    args = ap.parse_args()

    if args.show is not None:
        glsl, ubo, outs = make_case(args.seed * 100000 + args.show)
        print(glsl)
        return

    blob = bytearray(b"NXSF" + struct.pack("<II", 1, 0))
    n = 0
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(args.count):
            glsl, ubo, outs = make_case(args.seed * 100000 + k, args.op)
            dksh, err = compile_glsl(args.uam, glsl, tmp)
            if dksh is None:
                failed += 1
                print(f"prueba {k}: uam fallo:\n{err}\n{glsl}", file=sys.stderr)
                continue
            g = glsl.encode()
            blob += struct.pack("<I", len(g)) + pad4(g)
            blob += struct.pack("<I", len(dksh)) + pad4(dksh)
            blob += struct.pack("<32I", *ubo)
            for kind, exp, alt, tol in outs:
                blob += struct.pack("<IIIf", kind, exp, alt, tol)
            n += 1
    struct.pack_into("<I", blob, 8, n)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(blob))
    print(f"{n} pruebas ({failed} no compilaron) -> {out} ({len(blob) // 1024} KB)")


if __name__ == "__main__":
    main()
