// CPU de NeXo contra un ARM64 de referencia (QEMU) con instrucciones al azar.
//
// tests/generated/cpu_fuzz.bin lo genera tools/cpu_fuzz.py: para cada prueba guarda la
// instruccion, la semilla del estado inicial y la huella del estado final en QEMU.
// Aqui se ejecuta lo mismo en NeXo y se clasifica:
//   correcta        las dos la ejecutan y el resultado es identico
//   INCORRECTA      las dos la ejecutan pero el resultado es distinto  -> fallo del test
//   falta           QEMU la ejecuta y NeXo todavia no la implementa     -> cobertura
//   acepta invalida QEMU dice que no existe y NeXo la ejecuta           -> informativo
// Al final se imprime la cobertura de cada familia.
//
// Para depurar una prueba concreta: NEXO2_FUZZ_SHOW=<indice> imprime el estado final
// de NeXo, y "python3 tools/cpu_fuzz.py --show <indice>" el de QEMU.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "arm64/interpreter.hpp"
#include "arm64/fp_ops.hpp"
#include "common/logger.hpp"
#include "cpu_fuzz/cpu_fuzz_state.h"

#ifndef NEXO2_TEST_DATA_DIR
#define NEXO2_TEST_DATA_DIR "tests/generated"
#endif

using namespace NeXo2;

namespace {
struct FamilyStats { std::string name; int ok = 0, wrong = 0, missing = 0, invalid = 0, skipped = 0, fpsr = 0; };

bool LoadFuzz(std::vector<std::string>& names, std::vector<FuzzRecord>& records) {
    // NEXO2_FUZZ_FILE=<ruta>: usar otro archivo (p. ej. uno grande hecho con --count 5000 --output)
    const char* file_env = std::getenv("NEXO2_FUZZ_FILE");
    std::ifstream f(file_env ? std::string(file_env) : std::string(NEXO2_TEST_DATA_DIR) + "/cpu_fuzz.bin",
                    std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 16 || std::memcmp(d.data(), "NXFZ", 4) != 0) return false;
    size_t pos = 4;
    auto read32 = [&]() { u32 v; std::memcpy(&v, d.data() + pos, 4); pos += 4; return v; };
    read32();                                  // version
    const u32 families = read32();
    const u32 names_size = read32();
    const char* p = d.data() + pos;
    for (u32 i = 0; i < families; ++i) { names.emplace_back(p); p += names.back().size() + 1; }
    pos += names_size;
    const u32 count = read32();
    records.resize(count);
    if (d.size() < pos + count * sizeof(FuzzRecord)) return false;
    std::memcpy(records.data(), d.data() + pos, count * sizeof(FuzzRecord));
    return true;
}

void ToFuzzState(const Core::CPUState& s, FuzzState& out) {
    std::memset(&out, 0, sizeof(out));
    for (int i = 0; i < 31; ++i) out.x[i] = s.x[i];
    out.sp = s.sp;
    out.nzcv = s.GetNZCV();
    for (int i = 0; i < 32; ++i) { out.v[2 * i] = s.v[i].lo; out.v[2 * i + 1] = s.v[i].hi; }
    out.fpsr = static_cast<u32>(s.fpsr);
}

void PrintState(u32 instr, const Core::CPUState& s) {
    std::printf("NeXo: instr %08X\n", instr);
    for (int i = 0; i < 31; ++i)
        std::printf("x%-2d %016llX%s", i, (unsigned long long)s.x[i], i % 4 == 3 ? "\n" : "  ");
    std::printf("\nsp  %016llX  nzcv %llX  fpsr %08llX\n", (unsigned long long)s.sp,
                (unsigned long long)(s.GetNZCV() >> 28), (unsigned long long)s.fpsr);
    for (int i = 0; i < 32; ++i)
        std::printf("v%-2d %016llX%016llX%s", i, (unsigned long long)s.v[i].hi, (unsigned long long)s.v[i].lo,
                    i % 2 ? "\n" : "  ");
}
} // namespace

TEST(CpuFuzz_AgainstReferenceArm) {
    std::vector<std::string> names;
    std::vector<FuzzRecord> records;
    CHECK(LoadFuzz(names, records));
    if (records.empty()) return;

    const char* show_env = std::getenv("NEXO2_FUZZ_SHOW");
    const long show = show_env ? std::atol(show_env) : -1;
    // NEXO2_FUZZ_VERBOSE=1: lista tambien las que faltan y las no validas que NeXo acepta
    const bool verbose = std::getenv("NEXO2_FUZZ_VERBOSE") != nullptr;

    std::vector<FamilyStats> stats(names.size());
    for (size_t i = 0; i < names.size(); ++i) stats[i].name = names[i];

    Core::Memory mem;
    Core::Interpreter cpu(mem);
    Common::Logger::SetMuted(true);
    static u8 data[FUZZ_DATA_SIZE];
    int shown_wrong = 0, shown_fpsr = 0;

    // Se ejecuta todo dos veces: con la cache de instrucciones decodificadas (el camino
    // rapido, interpreter_fast.cpp) y sin ella (el decodificador normal). Los dos tienen
    // que coincidir con el ARM de referencia.
    auto run_all = [&](bool cache, std::vector<FamilyStats>& st) {
        cpu.SetDecodeCacheEnabled(cache);
        for (size_t idx = 0; idx < records.size(); ++idx) {
            const FuzzRecord& r = records[idx];
            FamilyStats& fs = st[r.family];
            if (r.result == FUZZ_FAULT) { ++fs.skipped; continue; }

            FuzzState in;
            fuzz_make_state(r.seed, r.memory, &in, data);
            mem.WriteBytes(FUZZ_DATA, data, FUZZ_DATA_SIZE);
            mem.Write<u32>(FUZZ_CODE, r.instr);
            Core::CPUState& s = cpu.GetState();
            s.Reset();
            for (int i = 0; i < 31; ++i) s.x[i] = in.x[i];
            s.sp = in.sp;
            s.SetNZCV(in.nzcv);
            for (int i = 0; i < 32; ++i) { s.v[i].lo = in.v[2 * i]; s.v[i].hi = in.v[2 * i + 1]; }
            s.fpcr = in.fpcr;
            s.fpsr = 0;
            s.pc = FUZZ_CODE;
            cpu.Resume();
            cpu.ClearExclusive();          // sin reserva de LDXR de la prueba anterior
            Core::FP::ClearHostFlags();

            const bool executed = cpu.Run(1) == 1 && !cpu.IsHalted();
            Core::FP::FoldHostFlags(s.fpsr);   // lo mismo que hace MRS FPSR
            if ((long)idx == show && cache) PrintState(r.instr, s);

            if (r.result == FUZZ_ILLEGAL) {
                if (executed) {
                    ++fs.invalid;
                    if (verbose) std::printf("    acepta no valida #%zu (%s): instr 0x%08X\n", idx, fs.name.c_str(), r.instr);
                }
                continue;
            }
            if (!executed) {
                ++fs.missing;
                if (verbose) std::printf("    falta #%zu (%s): instr 0x%08X\n", idx, fs.name.c_str(), r.instr);
                continue;
            }

            FuzzState out;
            ToFuzzState(s, out);
            mem.ReadBytes(FUZZ_DATA, data, FUZZ_DATA_SIZE);
            const bool same = s.pc == FUZZ_CODE + 4 && fuzz_hash(&out, data) == r.hash;
            if (same) {
                ++fs.ok;
                if (out.fpsr != r.fpsr) {
                    ++fs.fpsr;
                    if (shown_fpsr++ < 12)
                        std::printf("    FPSR distinto #%zu (%s): instr 0x%08X  NeXo %08X  ARM %08X\n", idx,
                                    fs.name.c_str(), r.instr, out.fpsr, r.fpsr);
                }
            } else {
                ++fs.wrong;
                if (shown_wrong++ < 12)
                    std::printf("    INCORRECTA #%zu (%s): instr 0x%08X\n", idx, fs.name.c_str(), r.instr);
            }
        }
    };
    run_all(true, stats);
    std::vector<FamilyStats> plain(names.size());
    for (size_t i = 0; i < names.size(); ++i) plain[i].name = names[i];
    run_all(false, plain);
    cpu.SetDecodeCacheEnabled(true);
    int plain_bad = 0;
    for (size_t i = 0; i < plain.size(); ++i) {
        const FamilyStats& f = plain[i];
        const int bad = f.wrong + f.fpsr + f.invalid + f.missing - stats[i].missing;
        if (bad) std::printf("    sin cache: %s tiene %d diferencias\n", names[i].c_str(), bad);
        plain_bad += bad;
    }
    Common::Logger::SetMuted(false);

    std::printf("    %-40s %8s %8s %6s %10s %9s %6s\n", "familia", "correcta", "INCORR.", "falta", "cobertura",
                "acepta-inv", "FPSR");
    int total_ok = 0, total_valid = 0, total_wrong = 0, total_invalid = 0, total_fpsr = 0;
    for (const auto& f : stats) {
        const int valid = f.ok + f.wrong + f.missing;
        total_ok += f.ok; total_valid += valid; total_wrong += f.wrong;
        total_invalid += f.invalid; total_fpsr += f.fpsr;
        std::printf("    %-40s %8d %8d %6d %9.1f%% %9d %6d\n", f.name.c_str(), f.ok, f.wrong, f.missing,
                    valid ? 100.0 * f.ok / valid : 100.0, f.invalid, f.fpsr);
    }
    std::printf("    TOTAL: %d de %d instrucciones validas correctas (%.1f%%)\n", total_ok, total_valid,
                total_valid ? 100.0 * total_ok / total_valid : 100.0);
    CHECK_EQ(total_wrong, 0);     // resultado distinto al de un ARM real
    CHECK_EQ(total_fpsr, 0);      // flags de coma flotante distintos
    CHECK_EQ(total_invalid, 0);   // NeXo ejecuta algo que en ARM no es valido
    CHECK_EQ(plain_bad, 0);       // el camino sin cache tiene que dar lo mismo
}
