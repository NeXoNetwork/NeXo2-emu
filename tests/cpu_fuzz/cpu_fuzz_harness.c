/* Arnes para QEMU (ARM64 Linux): ejecuta UNA instruccion con un estado concreto
 * y guarda como queda todo despues. Lo usa tools/cpu_fuzz.py.
 *
 * Truco: el kernel guarda TODOS los registros al entrar en un manejador de senal
 * (ucontext) y los vuelve a cargar al salir. Asi que:
 *   1. raise(SIGUSR1): el manejador cambia los registros guardados por el estado de
 *      la prueba (x0-x30, sp, NZCV, v0-v31) y el PC por la pagina de codigo.
 *   2. Al volver, la CPU ejecuta la instruccion. Detras hay un "udf" -> SIGILL.
 *   3. El manejador de SIGILL copia el estado final y restaura el contexto del
 *      paso 1: el programa sigue como si raise() hubiera vuelto normalmente.
 * Si la instruccion no es valida, el SIGILL llega en la propia instruccion; si
 * accede fuera de los 4 KB de datos, llega SIGSEGV.
 *
 * Uso: cpu_fuzz_harness entrada.bin salida.bin [indice_a_volcar]
 *   entrada: FuzzRecord con instr/family/memory/seed; salida: con result/hash/fpsr.
 */
#define _GNU_SOURCE
#include <asm/sigcontext.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "cpu_fuzz_state.h"

/* Los manejadores de senal escriben aqui: 'volatile' + barreras para que el
 * compilador no suponga que raise() no puede cambiarlas */
static FuzzState g_in, g_out;
static volatile int g_result;
static ucontext_t g_saved;

static struct fpsimd_context* find_fpsimd(ucontext_t* uc) {
    struct _aarch64_ctx* h = (struct _aarch64_ctx*)uc->uc_mcontext.__reserved;
    while (h->magic) {
        if (h->magic == FPSIMD_MAGIC) return (struct fpsimd_context*)h;
        h = (struct _aarch64_ctx*)((char*)h + h->size);
    }
    return NULL;
}

static void on_start(int sig, siginfo_t* si, void* ctx) {
    (void)sig; (void)si;
    ucontext_t* uc = (ucontext_t*)ctx;
    memcpy(&g_saved, uc, sizeof(*uc));
    for (int i = 0; i < 31; ++i) uc->uc_mcontext.regs[i] = g_in.x[i];
    uc->uc_mcontext.sp = g_in.sp;
    uc->uc_mcontext.pc = FUZZ_CODE;
    uc->uc_mcontext.pstate = (uc->uc_mcontext.pstate & ~0xF0000000ull) | g_in.nzcv;
    struct fpsimd_context* fp = find_fpsimd(uc);
    fp->fpsr = 0;
    fp->fpcr = g_in.fpcr;
    memcpy(fp->vregs, g_in.v, sizeof(g_in.v));
    /* Sin reserva exclusiva (LDXR) de una prueba anterior: si no, un STXR suelto
     * podria "funcionar" segun lo que se ejecuto antes. */
    __asm__ volatile("clrex" ::: "memory");
}

static void on_trap(int sig, siginfo_t* si, void* ctx) {
    (void)si;
    ucontext_t* uc = (ucontext_t*)ctx;
    const unsigned long long pc = uc->uc_mcontext.pc;
    if (sig == SIGILL && pc == FUZZ_CODE + 4) {
        for (int i = 0; i < 31; ++i) g_out.x[i] = uc->uc_mcontext.regs[i];
        g_out.sp = uc->uc_mcontext.sp;
        g_out.nzcv = uc->uc_mcontext.pstate & 0xF0000000ull;
        struct fpsimd_context* fp = find_fpsimd(uc);
        memcpy(g_out.v, fp->vregs, sizeof(g_out.v));
        g_out.fpsr = fp->fpsr;
        g_out.fpcr = fp->fpcr;
        g_result = FUZZ_OK;
    } else if ((sig == SIGILL || sig == SIGTRAP) && pc == FUZZ_CODE) {
        g_result = FUZZ_ILLEGAL;
    } else {
        g_result = FUZZ_FAULT;     /* memoria fuera de rango, salto a otro sitio... */
    }
    memcpy(uc, &g_saved, sizeof(*uc));   /* volver a donde se hizo raise() */
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s entrada salida [volcar]\n", argv[0]); return 1; }
    const long dump = argc > 3 ? atol(argv[3]) : -1;

    void* code  = mmap((void*)FUZZ_CODE, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    void* data  = mmap((void*)FUZZ_DATA, FUZZ_DATA_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    void* stack = mmap((void*)FUZZ_STACK, 0x2000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (code == MAP_FAILED || data == MAP_FAILED || stack == MAP_FAILED) { perror("mmap"); return 1; }

    /* Los manejadores usan su propia pila: la instruccion puede dejar SP en cualquier sitio */
    static char altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = on_start;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_sigaction = on_trap;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    FILE* in = fopen(argv[1], "rb");
    FILE* out = fopen(argv[2], "wb");
    if (!in || !out) { perror("fopen"); return 1; }

    static uint8_t initial[FUZZ_DATA_SIZE];
    FuzzRecord rec;
    long index = 0;
    while (fread(&rec, sizeof(rec), 1, in) == 1) {
        fuzz_make_state(rec.seed, rec.memory, &g_in, initial);
        memcpy(data, initial, FUZZ_DATA_SIZE);
        ((uint32_t*)code)[0] = rec.instr;
        ((uint32_t*)code)[1] = 0x00000000u;           /* udf #0 */
        __builtin___clear_cache((char*)code, (char*)code + 8);

        memset(&g_out, 0, sizeof(g_out));
        g_result = -1;
        __asm__ volatile("" ::: "memory");
        raise(SIGUSR1);
        __asm__ volatile("" ::: "memory");

        rec.result = (uint8_t)g_result;
        rec.hash = 0;
        rec.fpsr = 0;
        if (g_result == FUZZ_OK) {
            rec.hash = fuzz_hash(&g_out, (const uint8_t*)data);
            rec.fpsr = g_out.fpsr;
        }
        if (index == dump) {
            printf("instr %08X resultado %d\n", rec.instr, g_result);
            for (int i = 0; i < 31; ++i) printf("x%-2d %016llX%s", i, (unsigned long long)g_out.x[i], i % 4 == 3 ? "\n" : "  ");
            printf("\nsp  %016llX  nzcv %llX  fpsr %08X\n", (unsigned long long)g_out.sp,
                   (unsigned long long)(g_out.nzcv >> 28), g_out.fpsr);
            for (int i = 0; i < 32; ++i) printf("v%-2d %016llX%016llX%s", i, (unsigned long long)g_out.v[2 * i + 1],
                                                (unsigned long long)g_out.v[2 * i], i % 2 ? "\n" : "  ");
        }
        fwrite(&rec, sizeof(rec), 1, out);
        ++index;
    }
    fclose(in);
    fclose(out);
    return 0;
}
