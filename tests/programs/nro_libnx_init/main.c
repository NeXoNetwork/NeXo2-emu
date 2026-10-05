// Repite el arranque de libnx (__appInit, nx/source/runtime/init.c) paso a paso:
//   sm: -> apm -> appletOE (+ IApplicationProxy y sus interfaces) -> hid -> time:u -> fsp-srv
// Mismos servicios, comandos y formatos que usa libnx. Si NeXo pasa esta prueba,
// un homebrew real deberia llegar a su main().
#include "../nro_common/nx_min.h"

#define CUR_PROCESS_HANDLE 0xFFFF8001u

static u32 g_fail;
static void check(const char* what, u32 rc) {
    put(rc ? "FALLO " : "ok    "); put(what);
    if (rc) { put(" rc="); put_hex(rc); g_fail++; }
    flush();
}

int main(void) {
    put("Arranque estilo libnx en NeXo 2"); flush();
    u32 rc;

    // --- sm: ---
    u32 sm;
    check("svcConnectToNamedPort(sm:)", svcConnectToNamedPort(&sm, "sm:"));
    u64 zero = 0;
    IpcCall reg = { .cmd = 0, .in = &zero, .in_size = 8, .send_pid = 1 };
    check("sm: RegisterClient", ipc_call(sm, &reg));

    // --- apm (appletInitialize lo abre primero para aplicaciones) ---
    u32 apm, apm_session;
    check("sm: GetServiceHandle(apm)", sm_get_service(sm, "apm", &apm));
    IpcCall open = { .cmd = 0 };
    check("apm OpenSession", ipc_call(apm, &open));
    apm_session = open.out_handle;

    // --- appletOE (dominio) ---
    u32 am, am_root;
    check("sm: GetServiceHandle(appletOE)", sm_get_service(sm, "appletOE", &am));
    check("appletOE -> dominio", convert_to_domain(am, &am_root));
    IpcCall proxy = { .cmd = 0, .object_id = am_root, .in = &zero, .in_size = 8, .send_pid = 1,
                      .has_copy_handle = 1, .copy_handle = CUR_PROCESS_HANDLE };
    check("appletOE OpenApplicationProxy", ipc_call(am, &proxy));
    const u32 px = proxy.out_object;

    u32 funcs, creator, csg, self, win, audio, disp, dbg;
    check("IApplicationProxy GetApplicationFunctions", domain_open(am, px, 20, &funcs));
    check("IApplicationProxy GetLibraryAppletCreator", domain_open(am, px, 11, &creator));
    check("IApplicationProxy GetCommonStateGetter",    domain_open(am, px, 0, &csg));
    check("IApplicationProxy GetSelfController",       domain_open(am, px, 1, &self));
    check("IApplicationProxy GetWindowController",     domain_open(am, px, 2, &win));

    u64 aruid = 0;
    IpcCall c1 = { .cmd = 1, .object_id = win, .out = &aruid, .out_size = 8 };
    check("IWindowController GetAppletResourceUserId", ipc_call(am, &c1));

    check("IApplicationProxy GetAudioController",   domain_open(am, px, 3, &audio));
    check("IApplicationProxy GetDisplayController", domain_open(am, px, 4, &disp));
    check("IApplicationProxy GetDebugFunctions",    domain_open(am, px, 1000, &dbg));

    IpcCall ev1 = { .cmd = 91, .object_id = self };
    check("ISelfController GetAccumulatedSuspendedTickChangedEvent", ipc_call(am, &ev1));
    IpcCall ev2 = { .cmd = 0, .object_id = csg };
    check("ICommonStateGetter GetEventHandle", ipc_call(am, &ev2));

    u8 focus = 0;
    IpcCall fc = { .cmd = 9, .object_id = csg, .out = &focus, .out_size = 1 };
    check("ICommonStateGetter GetCurrentFocusState", ipc_call(am, &fc));
    IpcCall afr = { .cmd = 10, .object_id = win };
    check("IWindowController AcquireForegroundRights", ipc_call(am, &afr));
    u8 fhm[3] = {0, 0, 1};
    IpcCall sfh = { .cmd = 13, .object_id = self, .in = fhm, .in_size = 3 };
    check("ISelfController SetFocusHandlingMode", ipc_call(am, &sfh));
    u8 one = 1;
    IpcCall soo = { .cmd = 16, .object_id = self, .in = &one, .in_size = 1 };
    check("ISelfController SetOutOfFocusSuspendingEnabled", ipc_call(am, &soo));
    u8 running = 0;
    IpcCall nr = { .cmd = 40, .object_id = funcs, .out = &running, .out_size = 1 };
    check("IApplicationFunctions NotifyRunning", ipc_call(am, &nr));
    u8 opmode = 9; u32 perfmode = 9;
    IpcCall om = { .cmd = 5, .object_id = csg, .out = &opmode, .out_size = 1 };
    check("ICommonStateGetter GetOperationMode", ipc_call(am, &om));
    IpcCall pm = { .cmd = 6, .object_id = csg, .out = &perfmode, .out_size = 4 };
    check("ICommonStateGetter GetPerformanceMode", ipc_call(am, &pm));
    IpcCall n1 = { .cmd = 11, .object_id = self, .in = &one, .in_size = 1 };
    check("ISelfController SetOperationModeChangedNotification", ipc_call(am, &n1));
    IpcCall n2 = { .cmd = 12, .object_id = self, .in = &one, .in_size = 1 };
    check("ISelfController SetPerformanceModeChangedNotification", ipc_call(am, &n2));

    put("applet: aruid="); put_hex(aruid); put(" foco="); put_dec(focus);
    put(" modo="); put_dec(opmode); put(" running="); put_dec(running); flush();

    // --- hid ---
    u32 hid, hid_res;
    check("sm: GetServiceHandle(hid)", sm_get_service(sm, "hid", &hid));
    IpcCall car = { .cmd = 0, .in = &aruid, .in_size = 8, .send_pid = 1 };
    check("hid CreateAppletResource", ipc_call(hid, &car));
    hid_res = car.out_handle;
    IpcCall gsm = { .cmd = 0 };
    check("IAppletResource GetSharedMemoryHandle", ipc_call(hid_res, &gsm));
    const u64 hid_addr = 0x180000000ULL;
    check("svcMapSharedMemory(hid, 0x40000)", svcMapSharedMemory(gsm.out_handle, hid_addr, 0x40000, 1));
    MemoryInfo mi;
    svcQueryMemory(&mi, hid_addr);
    put("hid: memoria compartida tipo="); put_hex(mi.state); put(" tamano="); put_hex(mi.size); flush();

    // --- time:u ---
    u32 time, clk_user, clk_net, clk_steady, tz, clk_local;
    check("sm: GetServiceHandle(time:u)", sm_get_service(sm, "time:u", &time));
    IpcCall t0 = { .cmd = 0 }; check("time GetStandardUserSystemClock", ipc_call(time, &t0)); clk_user = t0.out_handle;
    IpcCall t1 = { .cmd = 1 }; check("time GetStandardNetworkSystemClock", ipc_call(time, &t1)); clk_net = t1.out_handle;
    IpcCall t2 = { .cmd = 2 }; check("time GetStandardSteadyClock", ipc_call(time, &t2)); clk_steady = t2.out_handle;
    IpcCall t3 = { .cmd = 3 }; check("time GetTimeZoneService", ipc_call(time, &t3)); tz = t3.out_handle;
    IpcCall t4 = { .cmd = 4 }; check("time GetStandardLocalSystemClock", ipc_call(time, &t4)); clk_local = t4.out_handle;
    IpcCall t20 = { .cmd = 20 }; check("time GetSharedMemoryNativeHandle", ipc_call(time, &t20));
    const u64 time_addr = 0x180040000ULL;
    check("svcMapSharedMemory(time, 0x1000)", svcMapSharedMemory(t20.out_handle, time_addr, 0x1000, 1));

    // Como timeGetCurrentTime(): offset del reloj local (0x38 + 8) + reloj continuo
    const u64 now = *(volatile u64*)(time_addr + 0x40);
    put("time: hora unix="); put_dec(now); flush();
    struct { u16 year; u8 month, day, hour, minute, second, pad; char info[0x18]; } cal;
    IpcCall tc = { .cmd = 101, .in = &now, .in_size = 8, .out = &cal, .out_size = sizeof(cal) };
    check("ITimeZoneService ToCalendarTimeWithMyRule", ipc_call(tz, &tc));
    put("time: fecha="); put_dec(cal.year); put("-"); put_dec(cal.month); put("-"); put_dec(cal.day);
    put(" zona="); put(cal.info + 8); flush();
    (void)clk_user; (void)clk_net; (void)clk_steady; (void)clk_local; (void)apm_session;

    // --- fsp-srv (dominio + sesiones clonadas, como sessionmgrCreate) ---
    u32 fs, fs_root, sdmc;
    check("sm: GetServiceHandle(fsp-srv)", sm_get_service(sm, "fsp-srv", &fs));
    check("fsp-srv -> dominio", convert_to_domain(fs, &fs_root));
    IpcCall scp = { .cmd = 1, .object_id = fs_root, .in = &zero, .in_size = 8, .send_pid = 1 };
    check("fsp-srv SetCurrentProcess", ipc_call(fs, &scp));
    cmif_request(CMIF_CONTROL, 2, 0, 0, 0, 0, 0);
    svcSendSyncRequest(fs);
    u32 fs_clone = 0;
    check("fsp-srv CloneCurrentObject", cmif_response(0, 0, 0, &fs_clone, 0));
    IpcCall sd = { .cmd = 18, .object_id = fs_root };
    check("fsp-srv OpenSdCardFileSystem (por la sesion clonada)", ipc_call(fs_clone, &sd));
    sdmc = sd.out_object;
    put("fs: IFileSystem de la SD = objeto "); put_dec(sdmc); flush();

    put(g_fail ? "Arranque con fallos: " : "__appInit completo, fallos: "); put_dec(g_fail); flush();
    rc = 0;
    return (int)rc;
}
