/* Estado inicial y huella (hash) de los tests de CPU contra un ARM de referencia.
 *
 * Este archivo se compila DOS veces:
 *   - en tools/cpu_fuzz_harness.c, para ARM64 Linux, que se ejecuta en QEMU
 *   - en tests/cpu_fuzz_tests.cpp, dentro de NeXo
 * Asi las dos CPUs empiezan cada prueba con exactamente el mismo estado
 * (generado a partir de una semilla) y calculan la huella del resultado igual.
 *
 * Es C normal (sin nada de C++) a proposito.
 */
#ifndef NEXO2_CPU_FUZZ_STATE_H
#define NEXO2_CPU_FUZZ_STATE_H
#include <stdint.h>
#include <string.h>

#define FUZZ_CODE   0x20000000ull   /* pagina donde va la instruccion a probar */
#define FUZZ_DATA   0x10000000ull   /* 4 KB de datos para lecturas/escrituras */
#define FUZZ_DATA_SIZE 0x1000u
#define FUZZ_STACK  0x30000000ull   /* pila para instrucciones que usan SP */

/* Resultado de una prueba en la CPU de referencia */
#define FUZZ_OK      0   /* se ejecuto: comparar la huella */
#define FUZZ_ILLEGAL 1   /* instruccion no valida (SIGILL) */
#define FUZZ_FAULT   2   /* fallo de memoria (fuera de los 4 KB): no se compara */

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t nzcv;        /* bits 31..28 */
    uint64_t v[64];       /* v0..v31: parte baja, parte alta */
    uint32_t fpsr;        /* flags acumulados de coma flotante (se compara aparte) */
    uint32_t fpcr;
} FuzzState;

static inline uint64_t fuzz_next(uint64_t* s) {      /* splitmix64 */
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Valores "interesantes" para coma flotante: ceros, unos, infinitos, NaN, subnormales... */
static const uint32_t FUZZ_F32[16] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBFC00000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u,
    0x00000001u, 0x807FFFFFu, 0x7F7FFFFFu, 0x40490FDBu, 0x4B000000u, 0xCF000000u, 0x3EAAAAABu, 0x5F000000u};
static const uint64_t FUZZ_F64[16] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF8000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull,
    0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0x400921FB54442D18ull,
    0x4330000000000000ull, 0xC3E0000000000000ull, 0x3FD5555555555555ull, 0x43E0000000000000ull};
static const uint64_t FUZZ_SPECIAL[8] = {
    0, 1, ~0ull, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull};

/* Estado inicial a partir de la semilla. 'memory' = 1 para instrucciones de memoria:
 * los registros apuntan casi siempre a la zona de datos. */
static inline void fuzz_make_state(uint64_t seed, int memory, FuzzState* st, uint8_t* data) {
    uint64_t s = seed;
    int i;
    memset(st, 0, sizeof(*st));
    for (i = 0; i < 31; ++i) {
        const uint64_t r = fuzz_next(&s) % 8;
        if (memory) {
            st->x[i] = (r < 2) ? (fuzz_next(&s) % 0x100)
                               : (FUZZ_DATA + 0x400 + (fuzz_next(&s) % 0x800)) & ~0xFull;
        } else switch (r) {
            case 0:  st->x[i] = fuzz_next(&s) % 0x100; break;
            case 1:  st->x[i] = FUZZ_SPECIAL[fuzz_next(&s) % 8]; break;
            case 2:  st->x[i] = fuzz_next(&s) & 0xFFFFFFFFull; break;
            case 3:  st->x[i] = (FUZZ_DATA + 0x400 + (fuzz_next(&s) % 0x800)) & ~0xFull; break;
            default: st->x[i] = fuzz_next(&s); break;
        }
    }
    st->sp = memory ? FUZZ_DATA + 0x800 : FUZZ_STACK + 0x800;
    st->nzcv = (fuzz_next(&s) & 0xF) << 28;
    for (i = 0; i < 32; ++i) {
        const uint64_t r = fuzz_next(&s) % 4;
        if (r == 0) {          /* 4 floats interesantes */
            st->v[2 * i]     = FUZZ_F32[fuzz_next(&s) % 16] | ((uint64_t)FUZZ_F32[fuzz_next(&s) % 16] << 32);
            st->v[2 * i + 1] = FUZZ_F32[fuzz_next(&s) % 16] | ((uint64_t)FUZZ_F32[fuzz_next(&s) % 16] << 32);
        } else if (r == 1) {   /* 2 doubles interesantes */
            st->v[2 * i]     = FUZZ_F64[fuzz_next(&s) % 16];
            st->v[2 * i + 1] = FUZZ_F64[fuzz_next(&s) % 16];
        } else {
            st->v[2 * i]     = fuzz_next(&s);
            st->v[2 * i + 1] = fuzz_next(&s);
        }
    }
    /* FPCR: la mitad de las veces 0 (lo normal); si no, modo de redondeo, FZ, DN,
     * AHP y FZ16 al azar, para probar todos los caminos de la coma flotante. */
    if (fuzz_next(&s) & 1) {
        const uint64_t r = fuzz_next(&s);
        st->fpcr = (uint32_t)(((r & 3) << 22) | (((r >> 2) & 1) << 24) | (((r >> 3) & 1) << 25) |
                              (((r >> 4) & 1) << 26) | (((r >> 5) & 1) << 19));
    }
    for (i = 0; i < (int)FUZZ_DATA_SIZE; i += 8) {
        const uint64_t r = fuzz_next(&s);
        memcpy(data + i, &r, 8);
    }
}

/* Huella FNV-1a de 64 bits de todo lo que la instruccion puede cambiar
 * (registros, NZCV, vectores y la zona de datos). FPSR/FPCR van aparte. */
static inline uint64_t fuzz_hash(const FuzzState* st, const uint8_t* data) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t* p = (const uint8_t*)st;
    const size_t n = sizeof(st->x) + sizeof(st->sp) + sizeof(st->nzcv) + sizeof(st->v);
    size_t i;
    for (i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    for (i = 0; i < FUZZ_DATA_SIZE; ++i) { h ^= data[i]; h *= 0x100000001B3ull; }
    return h;
}

/* Un resultado de referencia (formato de tests/generated/cpu_fuzz.bin) */
typedef struct {
    uint32_t instr;
    uint16_t family;
    uint8_t  memory;
    uint8_t  result;      /* FUZZ_OK / FUZZ_ILLEGAL / FUZZ_FAULT */
    uint64_t seed;
    uint64_t hash;        /* fuzz_hash del estado final */
    uint32_t fpsr;        /* FPSR final */
    uint32_t reserved;
} FuzzRecord;

#endif
