// Prueba de IPC para NeXo 2: habla con sm: y set:sys como lo haria libnx.
#include "../nro_common/nx_min.h"

typedef struct {
    u8 major, minor, micro, pad1, rev_major, rev_minor, pad2, pad3;
    char platform[0x20], hash[0x40], display_version[0x18], display_title[0x80];
} FirmwareVersion;

static FirmwareVersion g_fw;   // .bss: aqui escribe set:sys

static void print_rc(const char* what, u32 rc) { put(what); put(": rc="); put_hex(rc); flush(); }

int main(void) {
    put("Prueba IPC de NeXo 2"); flush();
    u32 rc;

    // 1) Conectar con el gestor de servicios
    u32 sm = 0;
    rc = svcConnectToNamedPort(&sm, "sm:");
    put("ConnectToNamedPort(sm:): rc="); put_hex(rc); put(" handle="); put_hex(sm); flush();

    u32 bad = 0;
    print_rc("ConnectToNamedPort(xyz)", svcConnectToNamedPort(&bad, "xyz"));

    // 2) RegisterClient (comando 0, con PID)
    u64* raw = (u64*)cmif_request(CMIF_REQUEST, 0, 8, 1, 0, 0, 0);
    raw[0] = 0;
    svcSendSyncRequest(sm);
    print_rc("sm:RegisterClient", cmif_response(0, 0, 0, 0, 0));

    // 3) QueryPointerBufferSize (comando de control 3)
    cmif_request(CMIF_CONTROL, 3, 0, 0, 0, 0, 0);
    svcSendSyncRequest(sm);
    void* out = 0;
    rc = cmif_response(0, 2, &out, 0, 0);
    put("QueryPointerBufferSize: rc="); put_hex(rc); put(" size="); put_hex(*(u16*)out); flush();

    // 4) GetServiceHandle("set:sys")
    raw = (u64*)cmif_request(CMIF_REQUEST, 1, 8, 0, 0, 0, 0);
    raw[0] = service_name("set:sys");
    svcSendSyncRequest(sm);
    u32 setsys = 0;
    rc = cmif_response(0, 0, 0, &setsys, 0);
    put("GetServiceHandle(set:sys): rc="); put_hex(rc); put(" handle="); put_hex(setsys); flush();

    // 5) set:sys GetFirmwareVersion (comando 3) -> buffer de salida tipo C
    cmif_request(CMIF_REQUEST, 3, 0, 0, &g_fw, sizeof(g_fw), 0);
    svcSendSyncRequest(setsys);
    rc = cmif_response(0, 0, 0, 0, 0);
    put("GetFirmwareVersion: rc="); put_hex(rc); put(" version=");
    put_dec(g_fw.major); put("."); put_dec(g_fw.minor); put("."); put_dec(g_fw.micro);
    put(" plataforma="); put(g_fw.platform); flush();

    // 6) Convertir set:sys en dominio y repetir la llamada a traves del dominio
    cmif_request(CMIF_CONTROL, 0, 0, 0, 0, 0, 0);
    svcSendSyncRequest(setsys);
    rc = cmif_response(0, 4, &out, 0, 0);
    const u32 object_id = *(u32*)out;
    put("ConvertCurrentObjectToDomain: rc="); put_hex(rc); put(" objeto="); put_dec(object_id); flush();

    memset(&g_fw, 0, sizeof(g_fw));
    cmif_request(CMIF_REQUEST, 4, 0, 0, &g_fw, sizeof(g_fw), object_id);
    svcSendSyncRequest(setsys);
    rc = cmif_response(1, 0, 0, 0, 0);
    put("Dominio -> GetFirmwareVersion2: rc="); put_hex(rc); put(" titulo="); put(g_fw.display_title); flush();

    // 7) El mismo GetServiceHandle por TIPC (como sm: en firmwares nuevos)
    raw = (u64*)tipc_request(1, 8);
    raw[0] = service_name("set:sys");
    svcSendSyncRequest(sm);
    u32 setsys2 = 0;
    rc = tipc_response(&setsys2);
    put("TIPC GetServiceHandle(set:sys): rc="); put_hex(rc); put(" handle="); put_hex(setsys2); flush();

    // 8) Cerrar handles y usar uno cerrado
    print_rc("CloseHandle(set:sys TIPC)", svcCloseHandle(setsys2));
    print_rc("SendSyncRequest(handle cerrado)", svcSendSyncRequest(setsys2));

    // 9) Un servicio que NeXo todavia no tiene: la CPU debe pararse diciendo cual
    raw = (u64*)cmif_request(CMIF_REQUEST, 1, 8, 0, 0, 0, 0);
    raw[0] = service_name("fsp-srv");
    svcSendSyncRequest(sm);
    u32 fs = 0;
    rc = cmif_response(0, 0, 0, &fs, 0);
    put("GetServiceHandle(fsp-srv): rc="); put_hex(rc); flush();

    put("Llamando a fsp-srv comando 1 (deberia parar la CPU)..."); flush();
    raw = (u64*)cmif_request(CMIF_REQUEST, 1, 8, 1, 0, 0, 0);
    raw[0] = 0;
    svcSendSyncRequest(fs);

    put("ERROR: no deberia llegar aqui"); flush();
    return 1;
}
