// Primer triangulo: un homebrew que dibuja con shaders, por el mismo camino que deko3d.
//   nvdrv -> memoria proyectada en la GPU -> canal -> pushbuffer con:
//   programas (shaders Maxwell compilados con uam), vertex buffer, atributos, viewport,
//   borrado y un draw de 3 vertices. Luego espera al fence y mira los pixeles.
#include "../nro_common/nx_min.h"
#include "../nro_common/nv_gpu.h"
#include "shaders.h"

#define CHANNEL_SYNCPT_INCR(id) ((id) | (1u << 20))
#define F_ONE  0x3F800000u
#define F_HALF 0x3F000000u
#define F_32   0x42000000u   /*  32.0 */
#define F_M32  0xC2000000u   /* -32.0 */
#define F_TENTH 0x3DCCCCCDu  /*  0.1 */

// Vertices: posicion (x, y, z) y color (r, g, b) en floats
static const u32 VERTS[18] = {
    0xBF800000u, 0xBF800000u, F_HALF,  F_ONE, 0, 0,        // (-1,-1) rojo
    F_ONE,       0xBF800000u, F_HALF,  0, F_ONE, 0,        // ( 1,-1) verde
    0,           F_ONE,       F_HALF,  0, 0, F_ONE,        // ( 0, 1) azul
};

static u32 px(const u8* rt, u32 x, u32 y) { return *(const u32*)(rt + y * 256 + x * 4); }

int main(void) {
    put("Primer triangulo de NeXo 2"); flush();

    u32 sm;
    check("svcConnectToNamedPort(sm:)", svcConnectToNamedPort(&sm, "sm:"));
    u64 zero = 0;
    IpcCall reg = { .cmd = 0, .in = &zero, .in_size = 8, .send_pid = 1 };
    check("sm: RegisterClient", ipc_call(sm, &reg));
    check("sm: GetServiceHandle(nvdrv)", sm_get_service(sm, "nvdrv", &g_nv));

    u32 nvmap, ctrl, as, gpu;
    check("Open /dev/nvmap", nv_open("/dev/nvmap", &nvmap));
    check("Open /dev/nvhost-ctrl", nv_open("/dev/nvhost-ctrl", &ctrl));
    check("Open /dev/nvhost-as-gpu", nv_open("/dev/nvhost-as-gpu", &as));
    check("Open /dev/nvhost-gpu", nv_open("/dev/nvhost-gpu", &gpu));
    u32 init[10] = {1, 0, 0x10000, 0};
    check("AS INITIALIZE_EX", nv_ioctl(as, IOWR(0x41, 0x09, 40), init, 40, 0, 0));

    // 512 KB del heap proyectados en la GPU
    u64 heap;
    check("svcSetHeapSize", svcSetHeapSize(&heap, 0x200000));
    u8* mem = (u8*)heap;
    const u32 SIZE = 0x80000;
    u32 create[2] = {SIZE, 0};
    check("NVMAP CREATE", nv_ioctl(nvmap, IOWR(0x01, 0x01, 8), create, 8, 0, 0));
    u32 alloc[8] = {create[1], 0, 0, 0x10000, 0, 0, 0, 0};
    *(u64*)&alloc[6] = heap;
    check("NVMAP ALLOC", nv_ioctl(nvmap, IOWR(0x01, 0x04, 32), alloc, 32, 0, 0));
    u32 map[10];
    memset(map, 0, sizeof(map));
    map[1] = 0xFFFFFFFF; map[2] = create[1]; map[3] = 0x10000;
    check("AS MAP_BUFFER_EX", nv_ioctl(as, IOWR(0x41, 0x06, 40), map, 40, 0, 0));
    const u64 iova = *(u64*)&map[8];

    u32 fdarg = nvmap;
    check("CHANNEL SET_NVMAP_FD", nv_ioctl(gpu, IOWR(0x48, 0x01, 4), &fdarg, 4, 0, 0));
    fdarg = gpu;
    check("AS BIND_CHANNEL", nv_ioctl(as, IOWR(0x41, 0x01, 4), &fdarg, 4, 0, 0));
    u32 gpfifo[8] = {0x800, 1, 0, 0, 0, 0, 0, 0};
    check("ALLOC_GPFIFO_EX2", nv_ioctl(gpu, IOWR(0x48, 0x1A, 32), gpfifo, 32, 0, 0));
    const u32 syncpt = gpfifo[3], fence0 = gpfifo[4];
    u32 obj[4] = {0xB197, 0, 0, 0};
    check("ALLOC_OBJ_CTX(3D)", nv_ioctl(gpu, IOWR(0x48, 0x09, 16), obj, 16, 0, 0));

    // Memoria: [pushbuffer][programas][vertices][render target 64x64 lineal]
    const u64 PB = iova, CODE = iova + 0x10000, VB = iova + 0x20000, RT = iova + 0x40000;
    memcpy(mem + 0x10000, tri_vert_code, TRI_VERT_SIZE);
    memcpy(mem + 0x11000, color_frag_code, COLOR_FRAG_SIZE);
    memcpy(mem + 0x20000, VERTS, sizeof(VERTS));
    g_pb = (u32*)mem;
    g_pbn = 0;

    cmd_mode(3, 0, 0, 1, (const u32[]){0xB197});
    CMD(0, 0x582, (u32)(CODE >> 32), (u32)CODE);              // region de programas
    CMD(0, 0x810, 1u | (1u << 4), TRI_VERT_ENTRY);            // shader de vertices
    CMD(0, 0x814, 0);
    CMD(0, 0x850, 1u | (5u << 4), 0x1000 + COLOR_FRAG_ENTRY); // shader de pixeles
    CMD(0, 0x854, 4);
    // Render target 0: 64x64 RGBA8 lineal (pitch 256)
    CMD(0, 0x200, (u32)(RT >> 32), (u32)RT, 256, 64, 0xD5, 1u << 12, 1, 0);
    CMD(0, 0x487, 1u | (076543210u << 4));
    CMD(0, 0x3FD, 64u << 16, 64u << 16);
    // Viewport (como deko3d): x = 32*x + 32, y = -32*y + 32
    CMD(0, 0x280, F_32, F_M32, F_ONE, F_32, F_32, 0);
    CMD(0, 0x300, 64u << 16, 64u << 16, 0, F_ONE);
    CMD(0, 0x64B, 1);
    CMD(0, 0x35F, 1);
    CMD(0, 0x680, 0x1111);
    // Vertex buffer 0 (24 bytes por vertice) y atributos 0 (posicion) y 1 (color): 3 floats
    CMD(0, 0x700, 24u | (1u << 12), (u32)(VB >> 32), (u32)VB);
    CMD(0, 0x458, (0x02u << 21) | (7u << 27), (12u << 7) | (0x02u << 21) | (7u << 27));
    // Culling como deko3d por defecto: delantera CCW, quitar traseras (el triangulo es CCW)
    CMD(0, 0x646, 1, 0x901, 0x405);
    // Fondo gris oscuro y dibujar 3 vertices como triangulo
    CMD(0, 0x360, F_TENTH, F_TENTH, F_TENTH, F_ONE);
    CMD(0, 0x674, 0xF << 2);
    CMD(0, 0x586, 4);
    CMD(0, 0x35D, 0, 3);
    CMD(0, 0x585, 0);
    CMD(0, 0xB2, CHANNEL_SYNCPT_INCR(syncpt));

    u32 kick[6] = {0, 0, 1, (1u << 2) | (1u << 8), 0, 1};
    u64 entry = PB | ((u64)g_pbn << 42) | (1ull << 41);
    check("KICKOFF_PB", nv_ioctl(gpu, IOWR(0x48, 0x1B, 24), kick, 24, &entry, 8));
    u32 wait[4] = {syncpt, fence0 + 1, 0xFFFFFFFF, 0};
    check("EVENT_WAIT_ASYNC (fence)", nv_ioctl(ctrl, IOWR(0x00, 0x1E, 16), wait, 16, 0, 0));

    // Pixeles (RGBA8: rojo en el byte bajo)
    const u8* rt = mem + 0x40000;
    expect("esquina (0,0): fondo", px(rt, 0, 0), 0xFF1A1A1Au);
    const u32 bl = px(rt, 1, 62), br = px(rt, 62, 62), top = px(rt, 32, 2), mid = px(rt, 32, 42);
    expect("abajo izquierda: rojo", (bl & 0xFF) > 230 && ((bl >> 8) & 0xFF) < 20, 1);
    expect("abajo derecha: verde", ((br >> 8) & 0xFF) > 230 && (br & 0xFF) < 20, 1);
    expect("arriba: azul", ((top >> 16) & 0xFF) > 220 && (top & 0xFF) < 20, 1);
    u32 r = mid & 0xFF, g = (mid >> 8) & 0xFF, b = (mid >> 16) & 0xFF;
    expect("centro: un tercio de cada color", r > 78 && r < 92 && g > 78 && g < 92 && b > 78 && b < 92, 1);
    u32 covered = 0;
    for (u32 y = 0; y < 64; y++)
        for (u32 x = 0; x < 64; x++)
            if (px(rt, x, y) != 0xFF1A1A1Au) covered++;
    put("pixeles del triangulo: "); put_dec(covered); flush();
    expect("area ~2048", covered > 2000 && covered < 2100, 1);

    put("triangulo completo, fallos: "); put_dec(g_fail); flush();
    return 0;
}
