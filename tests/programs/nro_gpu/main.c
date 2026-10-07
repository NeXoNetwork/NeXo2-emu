// Prueba de la GPU por el mismo camino que usan libnx y deko3d:
//   nvdrv -> /dev/nvmap, nvhost-ctrl, nvhost-ctrl-gpu, nvhost-as-gpu, nvhost-gpu
// Reserva memoria, la proyecta en la GPU, crea un canal, envia un pushbuffer
// (borrar un render target con una macro, recorte, copia DMA, syncpoint y semaforos)
// y comprueba en la memoria del programa lo que ha hecho la GPU.
#include "../nro_common/nx_min.h"
#include "../nro_common/nv_gpu.h"

// Macro FillRegisters de deko3d (ver tests/gpu_tests.cpp)
static const u32 MACRO_FILL[] = {0x00000A50, 0x00000300, 0xFFFFD211, 0xFFFFD017, 0x000018C0, 0x00000000};

#define CHANNEL_SYNCPT_INCR(id) ((id) | (1u << 20))
#define F_HALF 0x3F000000u
#define F_ONE  0x3F800000u

int main(void) {
    put("Prueba de la GPU de NeXo 2"); flush();

    u32 sm;
    check("svcConnectToNamedPort(sm:)", svcConnectToNamedPort(&sm, "sm:"));
    u64 zero = 0;
    IpcCall reg = { .cmd = 0, .in = &zero, .in_size = 8, .send_pid = 1 };
    check("sm: RegisterClient", ipc_call(sm, &reg));
    check("sm: GetServiceHandle(nvdrv)", sm_get_service(sm, "nvdrv", &g_nv));

    u32 nvmap, ctrl, ctrl_gpu, as, gpu;
    check("Open /dev/nvmap", nv_open("/dev/nvmap", &nvmap));
    check("Open /dev/nvhost-ctrl", nv_open("/dev/nvhost-ctrl", &ctrl));
    check("Open /dev/nvhost-ctrl-gpu", nv_open("/dev/nvhost-ctrl-gpu", &ctrl_gpu));
    check("Open /dev/nvhost-as-gpu", nv_open("/dev/nvhost-as-gpu", &as));
    check("Open /dev/nvhost-gpu", nv_open("/dev/nvhost-gpu", &gpu));

    // Caracteristicas de la GPU (nvGpuInit)
    u32 chars[4 + 40];
    memset(chars, 0, sizeof(chars));
    chars[0] = 0xA0; chars[2] = 1;
    check("GET_CHARACTERISTICS", nv_ioctl(ctrl_gpu, IOWR(0x47, 0x05, 16 + 0xA0), chars, 16 + 0xA0, 0, 0));
    expect("clase 3D", chars[4 + 92 / 4], 0xB197);
    expect("clase DMA", chars[4 + 108 / 4], 0xB0B5);

    // Espacio de direcciones (nvAddressSpaceCreate con pagina grande de 64 KB)
    u32 init[10] = {1, 0, 0x10000, 0};
    check("AS INITIALIZE_EX", nv_ioctl(as, IOWR(0x41, 0x09, 40), init, 40, 0, 0));

    // Memoria: 256 KB del heap -> nvmap -> proyectada en la GPU
    u64 heap;
    check("svcSetHeapSize", svcSetHeapSize(&heap, 0x200000));
    u8* mem = (u8*)heap;
    const u32 SIZE = 0x40000;
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
    expect("direccion GPU no nula", iova != 0, 1);

    // Canal (nvGpuChannelCreate)
    u32 fdarg = nvmap;
    check("CHANNEL SET_NVMAP_FD", nv_ioctl(gpu, IOWR(0x48, 0x01, 4), &fdarg, 4, 0, 0));
    fdarg = gpu;
    check("AS BIND_CHANNEL", nv_ioctl(as, IOWR(0x41, 0x01, 4), &fdarg, 4, 0, 0));
    u32 gpfifo[8] = {0x800, 1, 0, 0, 0, 0, 0, 0};
    check("ALLOC_GPFIFO_EX2", nv_ioctl(gpu, IOWR(0x48, 0x1A, 32), gpfifo, 32, 0, 0));
    const u32 syncpt = gpfifo[3], fence0 = gpfifo[4];
    u32 obj[4] = {0xB197, 0, 0, 0};
    check("ALLOC_OBJ_CTX(3D)", nv_ioctl(gpu, IOWR(0x48, 0x09, 16), obj, 16, 0, 0));

    // Disposicion de la memoria: [pushbuffer 64K][render target 64K][copia lineal][semaforos]
    const u64 PB = iova, RT = iova + 0x10000, LIN = iova + 0x20000, SEM = iova + 0x30000;
    u32* lin = (u32*)(mem + 0x20000);
    u32* sem = (u32*)(mem + 0x30000);
    g_pb = (u32*)mem;
    g_pbn = 0;

    cmd_mode(3, 0, 0, 1, (const u32[]){0xB197});              // subcanal 0 = 3D
    cmd_mode(3, 4, 0, 1, (const u32[]){0xB0B5});              // subcanal 4 = copia
    // Subir la macro y ponerla como macro 3
    CMD(0, 0x45, 0);
    cmd_mode(3, 0, 0x46, 6, MACRO_FILL);
    CMD(0, 0x47, 3);
    cmd_mode(3, 0, 0x48, 4, (const u32[]){0, 0, 0, 0});
    // Render target 0: 64x32 RGBA8 block linear (bloques de 4 GOBs)
    CMD(0, 0x200, (u32)(RT >> 32), (u32)RT, 64, 32, 0xD5, 2 << 4, 1, 0);
    CMD(0, 0x487, 1u | (076543210u << 4));
    CMD(0, 0x3FD, 64u << 16, 32u << 16);
    // Color de borrado con la macro (0.5 en los 4 componentes) y borrar
    cmd_mode(5, 0, 0xE06, 3, (const u32[]){0x360u | (1u << 12), 4, F_HALF});
    CMD(0, 0x674, 0xF << 2);
    // Un rectangulo rojo con scissor
    CMD(0, 0x43E, 0x100);
    CMD(0, 0x380, 1, 16u | (32u << 16), 8u | (16u << 16));
    CMD(0, 0x360, F_ONE, 0, 0, F_ONE);
    CMD(0, 0x674, 0xF << 2);
    // Copiar el render target a un buffer lineal (pitch 256) con el motor de copia
    CMD(4, 0x100, (u32)(RT >> 32), (u32)RT, (u32)(LIN >> 32), (u32)LIN, 0, 256, 256, 32);
    CMD(4, 0x1CA, (2u << 4) | (1u << 12), 256, 32, 1, 0, 0);
    CMD(4, 0x90, (u32)(SEM >> 32), (u32)(SEM + 16), 0x5EED);
    CMD(4, 0xC0, 2u | (1u << 8) | (1u << 9) | (1u << 3));
    // Fin: syncpoint +1 (como signalFence de deko3d) y semaforo del 3D
    CMD(0, 0xB2, CHANNEL_SYNCPT_INCR(syncpt));
    CMD(0, 0x6C0, (u32)(SEM >> 32), (u32)SEM, 0xC0FFEE, 0x10000000u);

    // Enviar (nvGpuChannelKickoff -> KICKOFF_PB con Ioctl2)
    u32 kick[6] = {0, 0, 1, (1u << 2) | (1u << 8), 0, 1};
    u64 entry = PB | ((u64)g_pbn << 42) | (1ull << 41);
    check("KICKOFF_PB", nv_ioctl(gpu, IOWR(0x48, 0x1B, 24), kick, 24, &entry, 8));

    // Esperar al fence (nvFenceWait -> EVENT_WAIT_ASYNC)
    u32 wait[4] = {syncpt, fence0 + 1, 0xFFFFFFFF, 0};
    check("EVENT_WAIT_ASYNC (fence)", nv_ioctl(ctrl, IOWR(0x00, 0x1E, 16), wait, 16, 0, 0));
    u32 rd[2] = {syncpt, 0};
    check("SYNCPT_READ", nv_ioctl(ctrl, IOWR(0x00, 0x14, 8), rd, 8, 0, 0));
    expect("syncpoint", rd[1], fence0 + 1);

    // Resultados
    expect("semaforo 3D", sem[0], 0xC0FFEE);
    expect("semaforo DMA", sem[4], 0x5EED);
    expect("pixel (0,0) gris", lin[0], 0x80808080u);
    expect("pixel (63,31) gris", lin[31 * 64 + 63], 0x80808080u);
    expect("pixel (16,8) rojo", lin[8 * 64 + 16], 0xFF0000FFu);
    expect("pixel (31,15) rojo", lin[15 * 64 + 31], 0xFF0000FFu);
    expect("pixel (32,15) gris", lin[15 * 64 + 32], 0x80808080u);

    put("gpu completo, fallos: "); put_dec(g_fail); flush();
    return 0;
}
