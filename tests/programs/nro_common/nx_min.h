// Mini "libnx" para los programas de prueba de NeXo 2.
//
// Llamadas al kernel (SVC), un printf muy simple y un cliente IPC (HIPC/CMIF/TIPC)
// escrito a mano siguiendo el mismo formato que libnx (nx/include/switch/sf/).
// Asi los tests ejercitan el emulador igual que un homebrew real, sin depender
// de devkitPro.
#pragma once

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

// memset/memcpy: el compilador puede generar llamadas a ellas aunque no las usemos.
// 'volatile' evita que convierta estos bucles en... llamadas a si mismas.
void* memset(void* dst, int c, unsigned long n) {
    volatile u8* d = (volatile u8*)dst;
    while (n--) *d++ = (u8)c;
    return dst;
}
void* memcpy(void* dst, const void* src, unsigned long n) {
    volatile u8* d = (volatile u8*)dst;
    const u8* s = (const u8*)src;
    while (n--) *d++ = *s++;
    return dst;
}

// ---------------------------------------------------------------------------
// SVC
// ---------------------------------------------------------------------------
static u32 svcOutputDebugString(const char* str, u64 size) {
    register u64 x0 __asm__("x0") = (u64)str;
    register u64 x1 __asm__("x1") = size;
    __asm__ volatile("svc 0x27" : "+r"(x0) : "r"(x1) : "memory");
    return (u32)x0;
}

static u32 svcCloseHandle(u32 handle) {
    register u64 x0 __asm__("x0") = handle;
    __asm__ volatile("svc 0x16" : "+r"(x0) : : "memory");
    return (u32)x0;
}

static u32 svcConnectToNamedPort(u32* out_handle, const char* name) {
    register u64 x0 __asm__("x0");
    register u64 x1 __asm__("x1") = (u64)name;
    __asm__ volatile("svc 0x1F" : "=r"(x0), "+r"(x1) : : "memory");
    *out_handle = (u32)x1;
    return (u32)x0;
}

static u32 svcSendSyncRequest(u32 handle) {
    register u64 x0 __asm__("x0") = handle;
    __asm__ volatile("svc 0x21" : "+r"(x0) : : "memory");
    return (u32)x0;
}

// Buffer de mensajes IPC: los primeros 0x100 bytes de la TLS (TPIDRRO_EL0)
static u32* ipc_buffer(void) {
    u64 tls;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tls));
    return (u32*)tls;
}

// ---------------------------------------------------------------------------
// Texto
// ---------------------------------------------------------------------------
static char g_line[256];
static u32  g_len;

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

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
#define CMIF_REQUEST  4
#define CMIF_CONTROL  5
#define SFCI 0x49434653
#define SFCO 0x4F434653

static u64 align16(u64 v) { return (v + 15) & ~15ULL; }

// Prepara una peticion CMIF en la TLS y devuelve donde escribir los argumentos.
//   type      CMIF_REQUEST o CMIF_CONTROL
//   send_pid  1 si el comando pide el PID del proceso
//   out_buf   buffer de salida tipo C (puntero), o 0
//   object_id 0 = sesion normal; >0 = objeto dentro de un dominio
static void* cmif_request(u32 type, u32 cmd, u32 raw_size, int send_pid,
                          void* out_buf, u32 out_size, u32 object_id) {
    u32* m = ipc_buffer();
    memset(m, 0, 0x100);
    const u32 num_c = out_buf ? 1 : 0;
    u32 w = 2;                                      // palabras de cabecera
    if (send_pid) w += 1 + 2;                       // cabecera especial + PID (8 bytes)
    const u32 content = (object_id ? 16 : 0) + 16 + raw_size;
    const u32 data_bytes = 16 + content + 2 * num_c; // +16 de margen para alinear (como libnx)
    const u32 num_words = (data_bytes + 3) / 4;

    m[0] = type;
    m[1] = num_words | ((num_c ? 2 + num_c : 0) << 10) | (send_pid ? 0x80000000u : 0);
    if (send_pid) m[2] = 1; // enviar PID (el kernel lo rellena)

    u8* start = (u8*)m + align16(w * 4);
    if (object_id) {
        start[0] = 1;                                // enviar mensaje
        *(u16*)(start + 2) = (u16)(16 + raw_size);
        *(u32*)(start + 4) = object_id;
        start += 16;
    }
    u32* hdr = (u32*)start;
    hdr[0] = SFCI; hdr[1] = 0; hdr[2] = cmd; hdr[3] = 0;

    if (num_c) { // lista de recepcion, justo detras de las palabras de datos
        u32* recv = m + w + num_words;
        recv[0] = (u32)(u64)out_buf;
        recv[1] = (u32)((u64)out_buf >> 32) | (out_size << 16);
    }
    return hdr + 4;
}

// Lee la respuesta CMIF. Devuelve el resultado del comando.
static u32 cmif_response(int is_domain, u32 out_size, void** out_data,
                         u32* out_move_handle, u32* out_object_id) {
    u32* m = ipc_buffer();
    u32 w = 2;
    u32 num_copy = 0, num_move = 0;
    if (m[1] & 0x80000000u) {
        const u32 sh = m[2];
        w = 3 + ((sh & 1) ? 2 : 0);
        num_copy = (sh >> 1) & 0xF;
        num_move = (sh >> 5) & 0xF;
        if (num_move && out_move_handle) *out_move_handle = m[w + num_copy];
        w += num_copy + num_move;
    }
    u8* start = (u8*)m + align16(w * 4);
    if (is_domain) start += 16;
    u32* hdr = (u32*)start;
    if (hdr[0] != SFCO) return 0xDEAD;
    if (out_data) *out_data = hdr + 4;
    if (is_domain && out_object_id) *out_object_id = *(u32*)((u8*)(hdr + 4) + out_size);
    return hdr[2];
}

// Peticion TIPC (protocolo ligero de sm:): type = 16 + comando, sin cabecera SFCI
static void* tipc_request(u32 cmd, u32 raw_size) {
    u32* m = ipc_buffer();
    memset(m, 0, 0x100);
    m[0] = 16 + cmd;
    m[1] = (raw_size + 3) / 4;
    return m + 2;
}

static u32 tipc_response(u32* out_move_handle) {
    u32* m = ipc_buffer();
    u32 w = 2;
    if (m[1] & 0x80000000u) {
        const u32 sh = m[2];
        const u32 num_copy = (sh >> 1) & 0xF, num_move = (sh >> 5) & 0xF;
        w = 3;
        if (num_move && out_move_handle) *out_move_handle = m[w + num_copy];
        w += num_copy + num_move;
    }
    return m[w]; // el resultado va primero
}

// Nombre de servicio empaquetado en un u64 ("set:sys")
static u64 service_name(const char* s) {
    u64 v = 0;
    for (int i = 0; i < 8 && s[i]; ++i) v |= (u64)(u8)s[i] << (i * 8);
    return v;
}

// ---------------------------------------------------------------------------
// Llamada IPC generica (CMIF), como serviceDispatch de libnx
// ---------------------------------------------------------------------------
typedef struct {
    u32 cmd;              // id del comando
    u32 object_id;        // 0 = sesion normal; >0 = objeto de un dominio
    const void* in;       // argumentos de entrada
    u32 in_size;
    int send_pid;         // enviar el PID
    int has_copy_handle;  // enviar un handle "copy"
    u32 copy_handle;
    void* out;            // donde copiar los datos de salida
    u32 out_size;
    // Resultados
    u32 out_handle;       // primer handle devuelto (copy o move)
    u32 out_object;       // objeto devuelto (solo en dominios)
} IpcCall;

static u32 ipc_call(u32 session, IpcCall* c) {
    u32* m = ipc_buffer();
    memset(m, 0, 0x100);
    const int special = c->send_pid || c->has_copy_handle;
    u32 w = 2;
    if (special) {
        m[2] = (c->send_pid ? 1 : 0) | ((c->has_copy_handle ? 1u : 0u) << 1);
        w = 3 + (c->send_pid ? 2 : 0);
        if (c->has_copy_handle) m[w++] = c->copy_handle;
    }
    const u32 content = (c->object_id ? 16 : 0) + 16 + c->in_size;
    const u32 num_words = (16 + content + 3) / 4;
    m[0] = CMIF_REQUEST;
    m[1] = num_words | (special ? 0x80000000u : 0);

    u8* start = (u8*)m + align16(w * 4);
    if (c->object_id) {
        start[0] = 1;
        *(u16*)(start + 2) = (u16)(16 + c->in_size);
        *(u32*)(start + 4) = c->object_id;
        start += 16;
    }
    u32* hdr = (u32*)start;
    hdr[0] = SFCI; hdr[1] = 0; hdr[2] = c->cmd; hdr[3] = 0;
    if (c->in_size) memcpy(hdr + 4, c->in, c->in_size);

    u32 rc = svcSendSyncRequest(session);
    if (rc) return rc;

    // Respuesta
    w = 2;
    if (m[1] & 0x80000000u) {
        const u32 sh = m[2];
        const u32 num_copy = (sh >> 1) & 0xF, num_move = (sh >> 5) & 0xF;
        w = 3 + ((sh & 1) ? 2 : 0);
        if (num_copy + num_move) c->out_handle = m[w];
        w += num_copy + num_move;
    }
    start = (u8*)m + align16(w * 4);
    if (c->object_id) start += 16;
    hdr = (u32*)start;
    if (hdr[0] != SFCO) return 0xDEAD;
    if (hdr[2]) return hdr[2];
    if (c->out_size) memcpy(c->out, hdr + 4, c->out_size);
    if (c->object_id) c->out_object = *(u32*)((u8*)(hdr + 4) + c->out_size);
    return 0;
}

// Pide un servicio a sm: (GetServiceHandle)
static u32 sm_get_service(u32 sm, const char* name, u32* out) {
    u64 n = service_name(name);
    IpcCall c = { .cmd = 1, .in = &n, .in_size = 8 };
    u32 rc = ipc_call(sm, &c);
    *out = c.out_handle;
    return rc;
}

// Convierte una sesion en dominio y devuelve el id del objeto principal
static u32 convert_to_domain(u32 session, u32* out_object) {
    cmif_request(CMIF_CONTROL, 0, 0, 0, 0, 0, 0);
    u32 rc = svcSendSyncRequest(session);
    if (rc) return rc;
    void* out = 0;
    rc = cmif_response(0, 4, &out, 0, 0);
    if (!rc) *out_object = *(u32*)out;
    return rc;
}

// Pide al objeto 'obj' de un dominio una sub-interfaz (comando 'cmd' sin argumentos)
static u32 domain_open(u32 session, u32 obj, u32 cmd, u32* out_obj) {
    IpcCall c = { .cmd = cmd, .object_id = obj };
    u32 rc = ipc_call(session, &c);
    *out_obj = c.out_object;
    return rc;
}

static u32 svcMapSharedMemory(u32 handle, u64 addr, u64 size, u32 perm) {
    register u64 x0 __asm__("x0") = handle;
    register u64 x1 __asm__("x1") = addr;
    register u64 x2 __asm__("x2") = size;
    register u64 x3 __asm__("x3") = perm;
    __asm__ volatile("svc 0x13" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (u32)x0;
}

typedef struct { u64 addr, size; u32 state, attr, perm, ipc_ref, dev_ref, pad; } MemoryInfo;

static u32 svcQueryMemory(MemoryInfo* info, u64 addr) {
    register u64 x0 __asm__("x0") = (u64)info;
    register u64 x1 __asm__("x1");
    register u64 x2 __asm__("x2") = addr;
    __asm__ volatile("svc 0x6" : "+r"(x0), "=r"(x1) : "r"(x2) : "memory");
    return (u32)x0;
}
