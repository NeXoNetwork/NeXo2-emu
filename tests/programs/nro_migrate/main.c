// Un hilo de prioridad alta hace "yield" en bucle (svcSleepThread(0)) esperando a otro de
// prioridad mas baja creado en su mismo nucleo, pero con una mascara que permite otros
// nucleos (como pthread_create en los ports de Android, p. ej. GTA Chinatown Wars).
// En Horizon un nucleo libre se lleva ese hilo y el programa sigue. Si NeXo no lo hace,
// el hilo de prioridad baja no corre nunca y el bucle no termina.
#include "../nro_common/nx_min.h"

typedef long long s64;
typedef int s32;

static u32 svcCreateThread(u32* out, void (*entry)(u64), u64 arg, void* stack_top, s32 prio, s32 core) {
    register u64 x0 __asm__("x0");
    register u64 x1 __asm__("x1") = (u64)entry;
    register u64 x2 __asm__("x2") = arg;
    register u64 x3 __asm__("x3") = (u64)stack_top;
    register u64 x4 __asm__("x4") = (u64)(s64)prio;
    register u64 x5 __asm__("x5") = (u64)(s64)core;
    __asm__ volatile("svc 0x08" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    *out = (u32)x1;
    return (u32)x0;
}
static u32 svcStartThread(u32 h) {
    register u64 x0 __asm__("x0") = h;
    __asm__ volatile("svc 0x09" : "+r"(x0) : : "memory");
    return (u32)x0;
}
static void svcExitThread(void) { __asm__ volatile("svc 0x0A" : : : "memory"); }
static void svcSleepThread(s64 ns) {
    register u64 x0 __asm__("x0") = (u64)ns;
    __asm__ volatile("svc 0x0B" : "+r"(x0) : : "memory");
}
static u32 svcSetThreadCoreMask(u32 h, s32 core, u64 mask) {
    register u64 x0 __asm__("x0") = h;
    register u64 x1 __asm__("x1") = (u64)(s64)core;
    register u64 x2 __asm__("x2") = mask;
    __asm__ volatile("svc 0x0F" : "+r"(x0) : "r"(x1), "r"(x2) : "memory");
    return (u32)x0;
}
static u32 svcGetCurrentProcessorNumber(void) {
    register u64 x0 __asm__("x0");
    __asm__ volatile("svc 0x10" : "=r"(x0) : : "memory");
    return (u32)x0;
}

static volatile u32 g_flag;
static volatile u32 g_core = 99;
static u8 g_stack[0x4000] __attribute__((aligned(16)));

static void worker(u64 arg) {
    (void)arg;
    g_core = svcGetCurrentProcessorNumber();
    g_flag = 1;
    svcExitThread();
}

int main(void) {
    u32 h = 0;
    u32 ok = svcCreateThread(&h, worker, 0, g_stack + sizeof(g_stack), 59, -2) == 0;
    ok = ok && svcSetThreadCoreMask(h, -1, 0x7) == 0;
    ok = ok && svcStartThread(h) == 0;
    u64 spins = 0;
    while (ok && !g_flag && spins < 2000000) { svcSleepThread(0); ++spins; }
    if (ok && g_flag) { put("migrar: ok, nucleo "); put_dec(g_core); }
    else put("migrar: FALLO");
    flush();
    return 0;
}
