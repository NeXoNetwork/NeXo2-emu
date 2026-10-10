#include "time.hpp"
#include "hle/kernel.hpp"
#include <chrono>
#include <cstring>

namespace NeXo2::HLE {

namespace {
// Identificador del "origen" del reloj. Debe coincidir en todas las estructuras.
constexpr u8 CLOCK_SOURCE_ID[16] = {'N','e','X','o','2','-','s','t','e','a','d','y','c','l','k','!'};

constexpr size_t TIME_SHARED_MEMORY_SIZE = 0x1000;

// Escribe un objeto en la memoria compartida de time con su formato:
// u32 contador + relleno, y luego DOS copias del objeto (el programa lee la copia contador & 1).
void WriteSharedObject(std::vector<u8>& mem, size_t offset, const void* obj, size_t size) {
    const u32 counter = 0;
    std::memcpy(mem.data() + offset, &counter, 4);
    std::memcpy(mem.data() + offset + 8, obj, size);
    std::memcpy(mem.data() + offset + 8 + size, obj, size);
}

// TimeStandardSteadyClockTimePointType: base_time + id del origen (0x18 bytes)
struct SteadyClockContext { s64 base_time; u8 source_id[16]; };
// TimeSystemClockContext: offset + (time_point + id del origen) (0x20 bytes)
struct SystemClockContext { s64 offset; s64 time_point; u8 source_id[16]; };
} // namespace

u64 HostUnixTime() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// Algoritmo "days from civil" (Howard Hinnant), sin depender de la zona horaria del PC
void ToCalendarTime(s64 t, CalendarTime& out, CalendarAdditionalInfo& info) {
    s64 days = t / 86400;
    s64 secs = t % 86400;
    if (secs < 0) { secs += 86400; days -= 1; }
    out.hour   = static_cast<u8>(secs / 3600);
    out.minute = static_cast<u8>((secs % 3600) / 60);
    out.second = static_cast<u8>(secs % 60);
    out.padding = 0;

    const s64 z = days + 719468;
    const s64 era = (z >= 0 ? z : z - 146096) / 146097;
    const s64 doe = z - era * 146097;
    const s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const s64 mp = (5 * doy + 2) / 153;
    const s64 d = doy - (153 * mp + 2) / 5 + 1;
    const s64 m = mp < 10 ? mp + 3 : mp - 9;
    const s64 y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    out.year = static_cast<u16>(y);
    out.month = static_cast<u8>(m);
    out.day = static_cast<u8>(d);

    std::memset(&info, 0, sizeof(info));
    info.day_of_week = static_cast<u32>(((days % 7) + 11) % 7); // 1970-01-01 fue jueves (4)
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int before_month[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    info.day_of_year = static_cast<u32>(before_month[m - 1] + d - 1 + ((leap && m > 2) ? 1 : 0));
    std::memcpy(info.timezone_name, "UTC", 4);
}

TimeService::TimeService(std::string name) : ServiceObject(std::move(name)) {
    RegisterCommand(0, "GetStandardUserSystemClock", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<SystemClock>("ISystemClock (usuario)"));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "GetStandardNetworkSystemClock", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<SystemClock>("ISystemClock (red)"));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(2, "GetStandardSteadyClock", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<SteadyClock>());
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(3, "GetTimeZoneService", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<TimeZoneService>());
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(4, "GetStandardLocalSystemClock", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<SystemClock>("ISystemClock (local)"));
        ctx.SetResult(Result::Success);
    });
    // GetSharedMemoryNativeHandle -> memoria compartida con los relojes ya rellenos
    RegisterCommand(20, "GetSharedMemoryNativeHandle", [](IpcContext& ctx) {
        std::shared_ptr<KSharedMemory> shmem;
        const u32 handle = ctx.GetKernel().CreateSharedMemory("time", TIME_SHARED_MEMORY_SIZE, &shmem);

        SteadyClockContext steady{};
        std::memcpy(steady.source_id, CLOCK_SOURCE_ID, 16);
        SystemClockContext clock{};
        clock.offset = static_cast<s64>(HostUnixTime()); // hora actual = offset + reloj continuo
        std::memcpy(clock.source_id, CLOCK_SOURCE_ID, 16);

        WriteSharedObject(shmem->data, 0x00, &steady, sizeof(steady)); // reloj continuo
        WriteSharedObject(shmem->data, 0x38, &clock, sizeof(clock));   // reloj local/usuario
        WriteSharedObject(shmem->data, 0x80, &clock, sizeof(clock));   // reloj de red

        ctx.PushCopyHandle(handle);
        ctx.SetResult(Result::Success);
    });
}

SystemClock::SystemClock(std::string name) : ServiceObject(std::move(name)) {
    RegisterCommand(0, "GetCurrentTime", [](IpcContext& ctx) {
        ctx.Push<u64>(HostUnixTime());
        ctx.SetResult(Result::Success);
    });
}

SteadyClock::SteadyClock() : ServiceObject("ISteadyClock") {
    RegisterCommand(0, "GetCurrentTimePoint", [](IpcContext& ctx) {
        ctx.Push<s64>(0);
        ctx.PushBytes(CLOCK_SOURCE_ID, 16);
        ctx.SetResult(Result::Success);
    });
}

TimeZoneService::TimeZoneService() : ServiceObject("ITimeZoneService") {
    RegisterCommand(0, "GetDeviceLocationName", [](IpcContext& ctx) {
        char name[0x24] = "UTC";
        ctx.PushBytes(name, sizeof(name));
        ctx.SetResult(Result::Success);
    });
    // ToCalendarTime(u64 tiempo, buffer con la regla) / ToCalendarTimeWithMyRule(u64 tiempo)
    //   -> CalendarTime + CalendarAdditionalInfo. Siempre UTC: la regla no se mira.
    auto to_calendar = [](IpcContext& ctx) {
        CalendarTime cal{};
        CalendarAdditionalInfo info{};
        ToCalendarTime(ctx.Pop<s64>(), cal, info);
        ctx.Push(cal);
        ctx.Push(info);
        ctx.SetResult(Result::Success);
    };
    RegisterCommand(100, "ToCalendarTime", to_calendar);
    RegisterCommand(101, "ToCalendarTimeWithMyRule", to_calendar);
    // ToPosixTime(CalendarTime, regla) / ToPosixTimeWithMyRule(CalendarTime)
    //   -> u32 cuantos + buffer de s64 (puede haber 2 en un cambio de hora; en UTC siempre 1)
    auto to_posix = [](IpcContext& ctx) {
        const CalendarTime cal = ctx.Pop<CalendarTime>();
        // Dias desde 1970-01-01 (algoritmo "days from civil" de H. Hinnant)
        const s64 y = s64(cal.year) - (cal.month <= 2 ? 1 : 0);
        const s64 era = (y >= 0 ? y : y - 399) / 400;
        const s64 yoe = y - era * 400;
        const s64 m = cal.month;
        const s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + cal.day - 1;
        const s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const s64 days = era * 146097 + doe - 719468;
        const s64 t = days * 86400 + s64(cal.hour) * 3600 + s64(cal.minute) * 60 + cal.second;
        ctx.WriteBuffer(&t, sizeof(t), 0);
        ctx.Push<u32>(1);
        ctx.SetResult(Result::Success);
    };
    RegisterCommand(201, "ToPosixTime", to_posix);
    RegisterCommand(202, "ToPosixTimeWithMyRule", to_posix);
}

} // namespace NeXo2::HLE
