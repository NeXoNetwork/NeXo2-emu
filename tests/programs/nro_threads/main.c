// Hilos de verdad: 4 hilos en 4 nucleos suman en un contador compartido.
//
// Prueba el planificador y la sincronizacion del kernel con el MISMO protocolo que
// usa libnx (nx/source/kernel/mutex.c y condvar.c):
//   - mutex: palabra = handle del dueno (+ bit 0x40000000 si hay hilos esperando);
//     si esta ocupado -> svcArbitrateLock; al soltar con esperas -> svcArbitrateUnlock
//   - variable de condicion: svcWaitProcessWideKeyAtomic / svcSignalProcessWideKey
// La seccion critica es lenta a proposito (lee, espera, escribe), para que el
// planificador cambie de hilo DENTRO de ella y los demas tengan que esperar el mutex.
// Si el mutex no funcionase, se perderian sumas y el total no cuadraria.
#include "../nro_common/nx_min.h"

typedef long long s64;
typedef int s32;

#define MAIN_THREAD_HANDLE 0x00010001u
#define CUR_THREAD_HANDLE  0xFFFF8000u
#define HAS_WAITERS        0x40000000u
#define NUM_WORKERS        4
#define ITERATIONS         300

// ---------------------------------------------------------------------------
// SVC de hilos
// ---------------------------------------------------------------------------
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
static u32 svcGetThreadPriority(s32* out, u32 h) {
    register u64 x0 __asm__("x0");
    register u64 x1 __asm__("x1") = h;
    __asm__ volatile("svc 0x0C" : "=r"(x0), "+r"(x1) : : "memory");
    *out = (s32)x1;
    return (u32)x0;
}
static u32 svcSetThreadPriority(u32 h, s32 prio) {
    register u64 x0 __asm__("x0") = h;
    register u64 x1 __asm__("x1") = (u64)(s64)prio;
    __asm__ volatile("svc 0x0D" : "+r"(x0) : "r"(x1) : "memory");
    return (u32)x0;
}
static u32 svcGetCurrentProcessorNumber(void) {
    register u64 x0 __asm__("x0");
    __asm__ volatile("svc 0x10" : "=r"(x0) : : "memory");
    return (u32)x0;
}
static u32 svcWaitSynchronization(s32* index, const u32* handles, s32 count, s64 timeout) {
    register u64 x0 __asm__("x0");
    register u64 x1 __asm__("x1") = (u64)handles;
    register u64 x2 __asm__("x2") = (u64)(s64)count;
    register u64 x3 __asm__("x3") = (u64)timeout;
    __asm__ volatile("svc 0x18" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3) : "memory");
    *index = (s32)x1;
    return (u32)x0;
}
static u32 svcArbitrateLock(u32 owner, u32* addr, u32 tag) {
    register u64 x0 __asm__("x0") = owner;
    register u64 x1 __asm__("x1") = (u64)addr;
    register u64 x2 __asm__("x2") = tag;
    __asm__ volatile("svc 0x1A" : "+r"(x0) : "r"(x1), "r"(x2) : "memory");
    return (u32)x0;
}
static u32 svcArbitrateUnlock(u32* addr) {
    register u64 x0 __asm__("x0") = (u64)addr;
    __asm__ volatile("svc 0x1B" : "+r"(x0) : : "memory");
    return (u32)x0;
}
static u32 svcWaitProcessWideKeyAtomic(u32* mutex, u32* key, u32 tag, s64 timeout) {
    register u64 x0 __asm__("x0") = (u64)mutex;
    register u64 x1 __asm__("x1") = (u64)key;
    register u64 x2 __asm__("x2") = tag;
    register u64 x3 __asm__("x3") = (u64)timeout;
    __asm__ volatile("svc 0x1C" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (u32)x0;
}
static void svcSignalProcessWideKey(u32* key, s32 count) {
    register u64 x0 __asm__("x0") = (u64)key;
    register u64 x1 __asm__("x1") = (u64)(s64)count;
    __asm__ volatile("svc 0x1D" : "+r"(x0) : "r"(x1) : "memory");
}
static u64 svcGetSystemTick(void) {
    register u64 x0 __asm__("x0");
    __asm__ volatile("svc 0x1E" : "=r"(x0) : : "memory");
    return x0;
}

// ---------------------------------------------------------------------------
// Mutex y variable de condicion (mismo algoritmo que libnx)
// ---------------------------------------------------------------------------
static void mutexLock(u32* m, u32 self) {
    for (;;) {
        u32 cur = 0;
        if (__atomic_compare_exchange_n(m, &cur, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        // Ocupado: marcar "hay esperas" y pedir al kernel que nos duerma
        if (!(cur & HAS_WAITERS)) {
            const u32 want = cur | HAS_WAITERS;
            if (!__atomic_compare_exchange_n(m, &cur, want, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue;
            cur = want;
        }
        svcArbitrateLock(cur & ~HAS_WAITERS, m, self);
        // El kernel nos lo da al despertar (o hay que reintentar)
        if ((__atomic_load_n(m, __ATOMIC_ACQUIRE) & ~HAS_WAITERS) == self) return;
    }
}
static void mutexUnlock(u32* m, u32 self) {
    u32 expected = self;
    if (!__atomic_compare_exchange_n(m, &expected, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        svcArbitrateUnlock(m);   // habia hilos esperando: el kernel elige el siguiente
}
static u32 condvarWait(u32* cv, u32* m, u32 self, s64 timeout) {
    return svcWaitProcessWideKeyAtomic(m, cv, self, timeout);
}

// ---------------------------------------------------------------------------
// Estado compartido
// ---------------------------------------------------------------------------
static u32 g_mutex, g_cv;
static volatile u32 g_counter, g_done;
static volatile u32 g_core[NUM_WORKERS];
static volatile u32 g_handle[NUM_WORKERS];
static u8 g_stacks[NUM_WORKERS][0x4000] __attribute__((aligned(16)));

static void worker(u64 index) {
    const u32 self = g_handle[index];
    g_core[index] = svcGetCurrentProcessorNumber();
    for (int i = 0; i < ITERATIONS; ++i) {
        mutexLock(&g_mutex, self);
        u32 v = g_counter;                                   // leer...
        for (volatile int k = 0; k < 20; ++k) { }            // ...tardar un poco...
        g_counter = v + 1;                                   // ...y escribir
        mutexUnlock(&g_mutex, self);
    }
    mutexLock(&g_mutex, self);
    g_done = g_done + 1;
    svcSignalProcessWideKey(&g_cv, 1);                       // avisar al principal
    mutexUnlock(&g_mutex, self);
    if (index == 0) svcExitThread();                         // uno sale con svcExitThread...
}                                                            // ...los demas volviendo (LR = ExitThread)

static u32 g_fail;
static void check(const char* what, int ok) {
    put(ok ? "ok    " : "FALLO "); put(what); flush();
    if (!ok) g_fail++;
}

int main(void) {
    put("Hilos en NeXo 2"); flush();
    const u32 self = MAIN_THREAD_HANDLE;

    // --- Prioridad del hilo actual ---
    s32 prio = -1;
    check("svcGetThreadPriority = 44", svcGetThreadPriority(&prio, CUR_THREAD_HANDLE) == 0 && prio == 44);

    // --- Crear 4 hilos, uno por nucleo ---
    int created = 1;
    for (u32 i = 0; i < NUM_WORKERS; ++i) {
        u32 h = 0;
        if (svcCreateThread(&h, worker, i, g_stacks[i] + sizeof(g_stacks[i]), 44, (s32)i) != 0) created = 0;
        g_handle[i] = h;
    }
    check("svcCreateThread x4", created);
    check("svcSetThreadPriority", svcSetThreadPriority(g_handle[3], 45) == 0);
    for (u32 i = 0; i < NUM_WORKERS; ++i) svcStartThread(g_handle[i]);

    // --- Esperar con la variable de condicion a que acaben todos ---
    mutexLock(&g_mutex, self);
    while (g_done < NUM_WORKERS) condvarWait(&g_cv, &g_mutex, self, -1);
    mutexUnlock(&g_mutex, self);
    put("contador = "); put_dec(g_counter); flush();
    check("mutex: ninguna suma perdida (4 x 300)", g_counter == NUM_WORKERS * ITERATIONS);

    int cores_ok = 1;
    for (u32 i = 0; i < NUM_WORKERS; ++i) if (g_core[i] != i) cores_ok = 0;
    check("cada hilo en su nucleo (0, 1, 2, 3)", cores_ok);

    // --- Esperar a que cada hilo termine (un hilo terminado esta "signaled") ---
    int joined = 1;
    for (u32 i = 0; i < NUM_WORKERS; ++i) {
        s32 idx = -1;
        if (svcWaitSynchronization(&idx, (const u32*)&g_handle[i], 1, -1) != 0 || idx != 0) joined = 0;
    }
    check("svcWaitSynchronization(hilo) al terminar", joined);

    // --- Dormir: el reloj avanza al menos 1 ms (31250 ticks a 31,25 MHz) ---
    const u64 t0 = svcGetSystemTick();
    svcSleepThread(1000000);
    check("svcSleepThread(1 ms)", svcGetSystemTick() - t0 >= 31250);

    // --- Plazo vencido: esperar un hilo que nunca arranca ---
    u32 idle = 0;
    svcCreateThread(&idle, worker, 0, g_stacks[0] + sizeof(g_stacks[0]), 44, 0);
    s32 idx = -1;
    check("svcWaitSynchronization: plazo vencido (0xEA01)",
          svcWaitSynchronization(&idx, &idle, 1, 1000000) == 0xEA01);

    // --- Variable de condicion con plazo: vuelve con el mutex otra vez nuestro ---
    mutexLock(&g_mutex, self);
    const u32 rc = condvarWait(&g_cv, &g_mutex, self, 1000000);
    check("condvar con plazo: 0xEA01 y mutex recuperado", rc == 0xEA01 && (g_mutex & ~HAS_WAITERS) == self);
    mutexUnlock(&g_mutex, self);

    put(g_fail ? "Hilos con fallos: " : "hilos completo, fallos: "); put_dec(g_fail); flush();
    return 0;
}
