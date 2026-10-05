// "Hola mundo" para NeXo 2: un homebrew minimo sin libnx.
// Habla con el kernel directamente con instrucciones SVC y escribe por
// svcOutputDebugString, que NeXo muestra en su ventana y en la consola.
typedef unsigned long long u64;
typedef unsigned int u32;

// ---------------------------------------------------------------------------
// Llamadas al kernel (mismos numeros y registros que en la consola real)
// ---------------------------------------------------------------------------
typedef struct { u64 addr, size; u32 state, attr, perm, ipc_ref, dev_ref, pad; } MemoryInfo;
typedef struct { u32 key, flags; u64 value[2]; } ConfigEntry;

static u32 svcOutputDebugString(const char* str, u64 size) {
    register u64 x0 __asm__("x0") = (u64)str;
    register u64 x1 __asm__("x1") = size;
    __asm__ volatile("svc 0x27" : "+r"(x0) : "r"(x1) : "memory");
    return (u32)x0;
}

static u32 svcSetHeapSize(u64* out_addr, u64 size) {
    register u64 x0 __asm__("x0");
    register u64 x1 __asm__("x1") = size;
    __asm__ volatile("svc 0x1" : "=r"(x0), "+r"(x1) : : "memory");
    *out_addr = x1;
    return (u32)x0;
}

static u32 svcQueryMemory(MemoryInfo* info, u64 addr) {
    register u64 x0 __asm__("x0") = (u64)info;
    register u64 x1 __asm__("x1");
    register u64 x2 __asm__("x2") = addr;
    __asm__ volatile("svc 0x6" : "+r"(x0), "=r"(x1) : "r"(x2) : "memory");
    return (u32)x0;
}

static u32 svcGetInfo(u64* out, u32 id0, u32 handle, u64 id1) {
    register u64 x0 __asm__("x0");
    register u64 x1 __asm__("x1") = id0;
    register u64 x2 __asm__("x2") = handle;
    register u64 x3 __asm__("x3") = id1;
    __asm__ volatile("svc 0x29" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3) : "memory");
    *out = x1;
    return (u32)x0;
}

// ---------------------------------------------------------------------------
// Mini "printf": construimos el texto en un buffer y lo enviamos de una vez
// ---------------------------------------------------------------------------
static char g_line[256];   // .bss
static u32  g_len;         // .bss
static int  g_counter = 41; // .data

static void put(const char* s) { while (*s && g_len < sizeof(g_line) - 1) g_line[g_len++] = *s++; }
static void put_dec(u64 v) {
    char tmp[24]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) { char c[2] = { tmp[--n], 0 }; put(c); }
}
static void put_hex(u64 v) {
    put("0x");
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        u32 d = (u32)(v >> i) & 0xF;
        if (d || started || i == 0) { char c[2] = { (char)(d < 10 ? '0' + d : 'A' + d - 10), 0 }; put(c); started = 1; }
    }
}
static void flush(void) { svcOutputDebugString(g_line, g_len); g_len = 0; }

static u64 fibonacci(u32 n) { u64 a = 0, b = 1; while (n--) { u64 t = a + b; a = b; b = t; } return a; }

// ---------------------------------------------------------------------------
int main(const ConfigEntry* config, u64 loader_magic) {
    put("Hola desde un NRO en NeXo 2!"); flush();

    // 1) Lista de configuracion del loader (Homebrew ABI)
    int entries = 0;
    const char* argv = "";
    for (const ConfigEntry* e = config; e->key != 0; ++e) {
        ++entries;
        if (e->key == 5) argv = (const char*)e->value[1];
    }
    put("Loader: X1="); put_hex(loader_magic); put(", entradas de config="); put_dec((u64)entries); flush();
    put("argv: "); put(argv); flush();

    // 2) Variables globales en .data y .bss
    g_counter++;
    put("Contador (.data): "); put_dec((u64)g_counter); flush();

    // 3) Heap: pedimos 2 MB al kernel y lo usamos
    u64 heap = 0;
    u32 rc = svcSetHeapSize(&heap, 0x200000);
    put("svcSetHeapSize: rc="); put_hex(rc); put(", heap en "); put_hex(heap); flush();
    u64* numbers = (u64*)heap;
    u64 sum = 0;
    for (u32 i = 0; i < 1000; ++i) numbers[i] = i;
    for (u32 i = 0; i < 1000; ++i) sum += numbers[i];
    put("Suma en el heap: "); put_dec(sum); flush();

    // 4) Preguntar al kernel por la memoria donde esta nuestro codigo
    MemoryInfo info;
    svcQueryMemory(&info, (u64)&main);
    put("QueryMemory(main): base="); put_hex(info.addr); put(" tipo="); put_hex(info.state);
    put(" permisos="); put_hex(info.perm); flush();

    // 5) svcGetInfo: donde esta la zona de heap
    u64 heap_region = 0;
    svcGetInfo(&heap_region, 4, 0xFFFF8001u, 0);
    put("GetInfo(HeapRegionAddress): "); put_hex(heap_region); flush();

    // 6) Un poco de calculo
    put("fibonacci(50) = "); put_dec(fibonacci(50)); flush();

    put("Adios! (main devuelve -> svcExitProcess)"); flush();
    return 0;
}
