#pragma once
#include "hle/service.hpp"

// "time:u" / "time:a" / "time:s": relojes y zona horaria.
// Usamos la hora real del PC. libnx lee la hora sobre todo de una memoria
// compartida (GetSharedMemoryNativeHandle), asi que la rellenamos al entregarla.
// Referencia: https://switchbrew.org/wiki/Glue_services#time:a.2C_time:s.2C_time:u
namespace NeXo2::HLE {

class TimeService final : public ServiceObject {
public:
    explicit TimeService(std::string name);
};

class SystemClock final : public ServiceObject { public: explicit SystemClock(std::string name); };
class SteadyClock final : public ServiceObject { public: SteadyClock(); };
class TimeZoneService final : public ServiceObject { public: TimeZoneService(); };

// Segundos desde 1970 (UTC) segun el reloj del PC
u64 HostUnixTime();

// Fecha y hora UTC a partir de segundos desde 1970 (sin zonas horarias por ahora)
struct CalendarTime {
    u16 year;
    u8  month, day, hour, minute, second, padding;
};
struct CalendarAdditionalInfo {
    u32  day_of_week;      // 0 = domingo
    u32  day_of_year;      // 0 = 1 de enero
    char timezone_name[8];
    u32  is_dst;
    s32  utc_offset;       // segundos respecto a UTC
};
void ToCalendarTime(s64 unix_time, CalendarTime& out, CalendarAdditionalInfo& info);

} // namespace NeXo2::HLE
