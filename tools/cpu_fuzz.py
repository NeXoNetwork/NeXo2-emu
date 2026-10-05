#!/usr/bin/env python3
"""
Pruebas de la CPU contra un ARM64 de referencia (QEMU), con instrucciones al azar.

    python3 tools/cpu_fuzz.py                 # genera tests/generated/cpu_fuzz.bin
    python3 tools/cpu_fuzz.py --count 2000    # mas pruebas por familia
    python3 tools/cpu_fuzz.py --show 1234     # estado final de la prueba 1234 en QEMU

Para cada familia de instrucciones (enteros, coma flotante, SIMD, memoria...) se
generan instrucciones con bits al azar, se ejecutan en QEMU (modelo cortex-a76,
ARMv8.2 como la CPU de la Switch 2) con un estado inicial que sale de una semilla,
y se guarda la huella (hash) del estado final. tests/cpu_fuzz_tests.cpp ejecuta lo
mismo en NeXo y compara. Ver docs/07-nexo-internals/cpu-fuzzing.md.

No hace falta para compilar NeXo: el .bin generado ya esta en el repo.
Requisitos (Linux/WSL): qemu-aarch64 (qemu-user) y aarch64-linux-gnu-gcc.
"""
import argparse
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
FUZZ_DIR = ROOT / "tests" / "cpu_fuzz"
OUTPUT = ROOT / "tests" / "generated" / "cpu_fuzz.bin"
QEMU_CPU = "cortex-a76"

# Familias = grupos de codificacion del manual de ARM ("Top-level encodings" y las
# tablas de cada grupo). Cada patron tiene 32 caracteres, del bit 31 al 0:
#   '0'/'1' = bit fijo, 'x' = al azar.
# Saltos y sistema (SVC, MRS, barreras...) no se prueban asi: cambian el flujo o el entorno.
PATTERNS = [
    # --- Enteros ---
    ("enteros: datos con inmediato",          "xxx100xxxxxxxxxxxxxxxxxxxxxxxxxx", 0),
    ("enteros: datos con registros",          "xxxx101xxxxxxxxxxxxxxxxxxxxxxxxx", 0),
    ("enteros: CRC32",                        "x0011010110xxxxx010xxxxxxxxxxxxx", 0),
    ("enteros: memoria",                      "xxxx100xxxxxxxxxxxxxxxxxxxxxxxxx", 1),
    # --- Coma flotante escalar ---
    ("FP: conversion con enteros",            "x0x11110xx1xxxxx000000xxxxxxxxxx", 0),
    ("FP: conversion con coma fija",          "x0x11110xx0xxxxxxxxxxxxxxxxxxxxx", 0),
    ("FP: 1 operando (FMOV/FABS/FSQRT/FCVT)", "00011110xx1xxxxxx10000xxxxxxxxxx", 0),
    ("FP: comparacion",                       "00011110xx1xxxxxxx1000xxxxxxxxxx", 0),
    ("FP: inmediato (FMOV #)",                "00011110xx1xxxxxxxx100xxxxxxxxxx", 0),
    ("FP: comparacion condicional",           "00011110xx1xxxxxxxxx01xxxxxxxxxx", 0),
    ("FP: 2 operandos (FADD/FMUL...)",        "00011110xx1xxxxxxxxx10xxxxxxxxxx", 0),
    ("FP: seleccion condicional",             "00011110xx1xxxxxxxxx11xxxxxxxxxx", 0),
    ("FP: 3 operandos (FMADD...)",            "00011111xxxxxxxxxxxxxxxxxxxxxxxx", 0),
    # --- SIMD vectorial ---
    ("SIMD: tres iguales",                    "0xx01110xx1xxxxxxxxxx1xxxxxxxxxx", 0),
    ("SIMD: tres distintos (largos)",         "0xx01110xx1xxxxxxxxx00xxxxxxxxxx", 0),
    ("SIMD: dos registros",                   "0xx01110xx10000xxxxx10xxxxxxxxxx", 0),
    ("SIMD: entre carriles (ADDV...)",        "0xx01110xx11000xxxxx10xxxxxxxxxx", 0),
    ("SIMD: copia (DUP/INS/UMOV)",            "0xx01110000xxxxx0xxxx1xxxxxxxxxx", 0),
    ("SIMD: permutar (UZP/ZIP/TRN)",          "0x001110xx0xxxxx0xxx10xxxxxxxxxx", 0),
    ("SIMD: EXT",                             "0x101110000xxxxx0xxxx0xxxxxxxxxx", 0),
    ("SIMD: TBL/TBX",                         "0x001110000xxxxx0xxx00xxxxxxxxxx", 0),
    ("SIMD: inmediato (MOVI...)",             "0xx0111100000xxxxxxxx1xxxxxxxxxx", 0),
    ("SIMD: desplazamiento inmediato",        "0xx011110xxxxxxxxxxxx1xxxxxxxxxx", 0),
    ("SIMD: por elemento",                    "0xx01111xxxxxxxxxxxxx0xxxxxxxxxx", 0),
    ("SIMD: FP16 tres iguales",               "0xx01110x10xxxxx00xxx1xxxxxxxxxx", 0),
    ("SIMD: FP16 dos registros",              "0xx01110x111100xxxxx10xxxxxxxxxx", 0),
    ("SIMD: extension (SDOT, SQRDMLAH)",      "0xx01110xx0xxxxx1xxxx1xxxxxxxxxx", 0),
    # --- SIMD escalar ---
    ("SIMD escalar: tres iguales",            "01x11110xx1xxxxxxxxxx1xxxxxxxxxx", 0),
    ("SIMD escalar: tres distintos",          "01x11110xx1xxxxxxxxx00xxxxxxxxxx", 0),
    ("SIMD escalar: dos registros",           "01x11110xx10000xxxxx10xxxxxxxxxx", 0),
    ("SIMD escalar: pares (ADDP/FADDP)",      "01x11110xx11000xxxxx10xxxxxxxxxx", 0),
    ("SIMD escalar: copia (DUP)",             "01011110000xxxxx000001xxxxxxxxxx", 0),
    ("SIMD escalar: desplazamiento",          "01x111110xxxxxxxxxxxx1xxxxxxxxxx", 0),
    ("SIMD escalar: por elemento",            "01x11111xxxxxxxxxxxxx0xxxxxxxxxx", 0),
    ("SIMD escalar: FP16 tres iguales",       "01x11110x10xxxxx00xxx1xxxxxxxxxx", 0),
    ("SIMD escalar: FP16 dos registros",      "01x11110x111100xxxxx10xxxxxxxxxx", 0),
    ("SIMD escalar: extension (SQRDMLAH)",    "01x11110xx0xxxxx1xxxx1xxxxxxxxxx", 0),
    # --- Criptografia ---
    ("cripto: AES",                           "010011100010100xxxxx10xxxxxxxxxx", 0),
    ("cripto: SHA tres registros",            "01011110000xxxxx0xxx00xxxxxxxxxx", 0),
    ("cripto: SHA dos registros",             "010111100010100xxxxx10xxxxxxxxxx", 0),
    # --- SIMD memoria ---
    ("SIMD memoria: LDR/STR/LDP/STP",         "xx1x11xxxxxxxxxxxxxxxxxxxxxxxxxx", 1),
    ("SIMD memoria: LD1-4/ST1-4 multiples",   "0x001100xx0xxxxxxxxxxxxxxxxxxxxx", 1),
    ("SIMD memoria: LD1-4 un elemento/LDxR",  "0x001101xxxxxxxxxxxxxxxxxxxxxxxx", 1),
]


def qemu_too_lax(instr: int) -> bool:
    """Codificaciones que el manual de ARM marca como no validas pero QEMU 8.2 ejecuta
    (ignora algun bit). Para estas se espera "no valida" en NeXo, como en una CPU real."""
    bit = lambda n: (instr >> n) & 1
    field = lambda lo, n: (instr >> lo) & ((1 << n) - 1)
    # FMAXV/FMINV/FMAXNMV/FMINNMV de 16 bits (U=0): el bit 22 tiene que ser 0
    if (instr & 0xBF3E0C00) == 0x0E300800 and field(12, 5) in (0b01100, 0b01111) and bit(22):
        return True
    # FADDP/FMAXP/FMINP/FMAXNMP/FMINNMP escalares de 16 bits (U=0): el bit 22 tiene que ser 0
    if (instr & 0xFF3E0C00) == 0x5E300800 and field(12, 5) in (0b01100, 0b01101, 0b01111) and bit(22):
        return True
    # MOVI/MVNI...: op=1 con o2=1 no existe (o2=1 solo es FMOV de 16 bits con op=0)
    if (instr & 0x9FF80400) == 0x0F000400 and bit(29) and bit(11):
        return True
    # FP16 escalar de dos registros: FABS/FNEG (a=1, opcode 01111) solo existen en vector
    if (instr & 0xDF7E0C00) == 0x5E780800 and bit(23) and field(12, 5) == 0b01111:
        return True
    return False


def qemu_untestable(instr: int) -> bool:
    """Instrucciones validas que no se pueden comparar con QEMU (se saltan):
    - PAC (PACIA, AUTIA...): la CPU de la Switch 2 las tiene; el cortex-a76 de QEMU no.
    - FCVTZS/FCVTZU escalares de 16 bits a coma fija: QEMU escribe 32 bits en vez de 16."""
    if (instr & 0xFFFF0000) == 0xDAC10000:
        return True
    if (instr & 0xDFF0FC00) == 0x5F10FC00:
        return True
    return False


def pattern_bits(p: str):
    assert len(p) == 32, f"patron de {len(p)} bits: {p}"
    mask = value = 0
    for i, c in enumerate(p):
        bit = 31 - i
        if c in "01":
            mask |= 1 << bit
            value |= int(c) << bit
    return mask, value


FAMILIES = [(name, *pattern_bits(p), mem) for name, p, mem in PATTERNS]

RECORD = struct.Struct("<IHBBQQII")   # FuzzRecord de cpu_fuzz_state.h
HEADER = struct.Struct("<4sII")        # "NXFZ", version, numero de familias


def build_harness(tmp: pathlib.Path) -> pathlib.Path:
    exe = tmp / "harness"
    cmd = ["aarch64-linux-gnu-gcc", "-O2", "-static", "-I", str(FUZZ_DIR),
           str(FUZZ_DIR / "cpu_fuzz_harness.c"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("Error compilando el arnes:\n" + r.stderr)
    return exe


def generate(count: int, rng: random.Random):
    records = []
    for fam, (_, mask, value, memory) in enumerate(FAMILIES):
        for i in range(count):
            instr = (rng.getrandbits(32) & ~mask & 0xFFFFFFFF) | value
            seed = (fam << 32) | i
            records.append((instr, fam, memory, 0, seed, 0, 0, 0))
    return records


def qemu_ok(exe, records, tmp) -> bool:
    inp, out = tmp / "probe_in.bin", tmp / "probe_out.bin"
    inp.write_bytes(b"".join(RECORD.pack(*r) for r in records))
    cmd = ["qemu-aarch64", "-cpu", QEMU_CPU, str(exe), str(inp), str(out)]
    return subprocess.run(cmd, capture_output=True).returncode == 0


def drop_qemu_crashes(exe, records, tmp):
    """Algunas codificaciones no validas hacen abortar a QEMU ("code should not be
    reached") en vez de dar "instruccion no valida": es un fallo de QEMU. Se buscan
    partiendo la lista por la mitad y se quitan."""
    if qemu_ok(exe, records, tmp):
        return records, []
    if len(records) == 1:
        return [], records
    half = len(records) // 2
    a, bad_a = drop_qemu_crashes(exe, records[:half], tmp)
    b, bad_b = drop_qemu_crashes(exe, records[half:], tmp)
    return a + b, bad_a + bad_b


def run_qemu(exe, records, tmp, dump=None):
    inp, out = tmp / "in.bin", tmp / "out.bin"
    inp.write_bytes(b"".join(RECORD.pack(*r) for r in records))
    cmd = ["qemu-aarch64", "-cpu", QEMU_CPU, str(exe), str(inp), str(out)]
    if dump is not None:
        cmd.append(str(dump))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("QEMU fallo:\n" + r.stdout + r.stderr)
    if dump is not None:
        print(r.stdout)
    data = out.read_bytes()
    return [RECORD.unpack_from(data, i * RECORD.size) for i in range(len(data) // RECORD.size)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=1000, help="pruebas por familia")
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--show", type=int, help="volcar el estado final de esta prueba")
    ap.add_argument("--output", default=str(OUTPUT))
    args = ap.parse_args()

    rng = random.Random(args.seed)
    records = generate(args.count, rng)
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        exe = build_harness(tmp)
        records, crashes = drop_qemu_crashes(exe, records, tmp)
        if args.show is not None:   # mismo indice que en el .bin (ya sin las que tumban a QEMU)
            run_qemu(exe, [records[args.show]], tmp, dump=0)
            return
        for r in crashes:
            print(f"  (QEMU aborta con 0x{r[0]:08X}: no se prueba)")
        results = run_qemu(exe, records, tmp)
        results = [(r[0], r[1], r[2], 1, r[4], 0, 0, 0) if qemu_too_lax(r[0]) else r for r in results]
        # Resultado 2 = "fallo de memoria": el test de NeXo se las salta
        results = [(r[0], r[1], r[2], 2, r[4], 0, 0, 0) if qemu_untestable(r[0]) else r for r in results]

    out = pathlib.Path(args.output)
    names = b"".join(name.encode("utf-8") + b"\0" for name, *_ in FAMILIES)
    with open(out, "wb") as f:
        f.write(HEADER.pack(b"NXFZ", 1, len(FAMILIES)))
        f.write(struct.pack("<I", len(names)))
        f.write(names)
        f.write(struct.pack("<I", len(results)))
        for r in results:
            f.write(RECORD.pack(*r))

    print(f"{out.relative_to(ROOT) if out.is_relative_to(ROOT) else out}: {len(results)} pruebas")
    for fam, (name, *_) in enumerate(FAMILIES):
        rs = [r for r in results if r[1] == fam]
        ok = sum(1 for r in rs if r[3] == 0)
        ill = sum(1 for r in rs if r[3] == 1)
        print(f"  {name:42s} validas {ok:5d}  no validas {ill:5d}  fallo de memoria {len(rs) - ok - ill:5d}")


if __name__ == "__main__":
    main()
