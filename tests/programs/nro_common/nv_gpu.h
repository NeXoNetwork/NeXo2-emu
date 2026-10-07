// Ayudas comunes de los homebrew de prueba de la GPU: llamar a nvdrv por IPC (como
// libnx) y escribir comandos de la GPU en un pushbuffer (como deko3d).
// Lo usan nro_gpu y nro_triangle.
#pragma once
#include "nx_min.h"
static u32 g_fail;
static void check(const char* what, u32 rc) {
    put(rc ? "FALLO " : "ok    "); put(what);
    if (rc) { put(" rc="); put_hex(rc); g_fail++; }
    flush();
}
static void expect(const char* what, u64 got, u64 want) {
    put(got == want ? "ok    " : "FALLO "); put(what);
    if (got != want) { put(" = "); put_hex(got); put(", esperado "); put_hex(want); g_fail++; }
    flush();
}

static u32 svcSetHeapSize(u64* out_addr, u64 size) {
    register u64 x0 __asm__("x0");
    register u64 x1 __asm__("x1") = size;
    __asm__ volatile("svc 0x1" : "=r"(x0), "+r"(x1) : : "memory");
    *out_addr = x1;
    return (u32)x0;
}

// ---------------------------------------------------------------------------
// IPC con buffers A (entrada) y B (salida), como nvIoctl de libnx
// ---------------------------------------------------------------------------
typedef struct { const void* ptr; u32 size; } Buf;

static void put_desc(u32* d, const void* ptr, u32 size) {
    const u64 addr = (u64)ptr;
    d[0] = size;
    d[1] = (u32)addr;
    d[2] = (u32)(((addr >> 36) & 7) << 2) | (u32)(((addr >> 32) & 0xF) << 28);
}

// Comando CMIF con hasta 2 buffers A y 1 B. Devuelve el resultado; copia la salida.
static u32 call_buf(u32 session, u32 cmd, const void* raw, u32 raw_size, Buf a0, Buf a1, Buf b0,
                    void* out, u32 out_size, u32* out_handle) {
    u32* m = ipc_buffer();
    memset(m, 0, 0x100);
    const u32 num_a = (a0.ptr ? 1 : 0) + (a1.ptr ? 1 : 0), num_b = b0.ptr ? 1 : 0;
    const u32 content = 16 + raw_size;
    const u32 num_words = (16 + content + 3) / 4;
    m[0] = CMIF_REQUEST | (num_a << 20) | (num_b << 24);
    m[1] = num_words;
    u32 w = 2;
    if (a0.ptr) { put_desc(m + w, a0.ptr, a0.size); w += 3; }
    if (a1.ptr) { put_desc(m + w, a1.ptr, a1.size); w += 3; }
    if (b0.ptr) { put_desc(m + w, b0.ptr, b0.size); w += 3; }
    u32* hdr = (u32*)((u8*)m + align16(w * 4));
    hdr[0] = SFCI; hdr[1] = 0; hdr[2] = cmd; hdr[3] = 0;
    if (raw_size) memcpy(hdr + 4, raw, raw_size);

    u32 rc = svcSendSyncRequest(session);
    if (rc) return rc;
    w = 2;
    if (m[1] & 0x80000000u) {
        const u32 sh = m[2];
        const u32 num_copy = (sh >> 1) & 0xF, num_move = (sh >> 5) & 0xF;
        w = 3 + ((sh & 1) ? 2 : 0);
        if (out_handle && num_copy + num_move) *out_handle = m[w];
        w += num_copy + num_move;
    }
    hdr = (u32*)((u8*)m + align16(w * 4));
    if (hdr[0] != SFCO) return 0xDEAD;
    if (hdr[2]) return hdr[2];
    if (out_size) memcpy(out, hdr + 4, out_size);
    return 0;
}

static const Buf NONE = {0, 0};
static u32 g_nv;

static u32 nv_open(const char* path, u32* fd) {
    u32 len = 0;
    while (path[len]) len++;
    u32 out[2] = {0, 0};
    u32 rc = call_buf(g_nv, 0, 0, 0, (Buf){path, len + 1}, NONE, NONE, out, 8, 0);
    *fd = out[0];
    return rc ? rc : out[1];
}

// Ioctl (version 1) o Ioctl2 (datos extra de entrada en un segundo buffer A)
static u32 nv_ioctl(u32 fd, u32 request, void* data, u32 size, const void* extra, u32 extra_size) {
    u32 raw[2] = {fd, request};
    u32 err = 0;
    u32 rc = extra ? call_buf(g_nv, 11, raw, 8, (Buf){data, size}, (Buf){extra, extra_size}, (Buf){data, size}, &err, 4, 0)
                   : call_buf(g_nv, 1, raw, 8, (Buf){data, size}, NONE, (Buf){data, size}, &err, 4, 0);
    return rc ? rc : err;
}
#define IOWR(type, nr, size) ((3u << 30) | ((u32)(size) << 16) | ((u32)(type) << 8) | (nr))

// ---------------------------------------------------------------------------
// Comandos de la GPU (cabeceras de metodo de Maxwell, como deko3d)
// ---------------------------------------------------------------------------
static u32* g_pb;
static u32  g_pbn;
static u32 header(u32 mode, u32 count, u32 subch, u32 method) {
    return (method & 0x1FFF) | ((subch & 7) << 13) | ((count & 0x1FFF) << 16) | (mode << 29);
}
static void cmd(u32 subch, u32 method, u32 n, const u32* args) {   // incrementando
    g_pb[g_pbn++] = header(1, n, subch, method);
    for (u32 i = 0; i < n; i++) g_pb[g_pbn++] = args[i];
}
static void cmd_mode(u32 mode, u32 subch, u32 method, u32 n, const u32* args) {
    g_pb[g_pbn++] = header(mode, n, subch, method);
    for (u32 i = 0; i < n; i++) g_pb[g_pbn++] = args[i];
}
#define CMD(subch, method, ...) do { const u32 a_[] = {__VA_ARGS__}; cmd(subch, method, sizeof(a_) / 4, a_); } while (0)
