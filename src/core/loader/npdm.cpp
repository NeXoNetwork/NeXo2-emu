#include "npdm.hpp"
#include <cstring>

namespace NeXo2::Loader {

namespace {
constexpr u32 META_MAGIC = 0x4154454D;   // "META"
constexpr u32 ACI0_MAGIC = 0x30494341;   // "ACI0"

template <typename T>
T Get(const std::vector<u8>& f, size_t off) {
    T v{};
    if (off + sizeof(T) <= f.size()) std::memcpy(&v, f.data() + off, sizeof(T));
    return v;
}
} // namespace

NpdmResult ParseNpdm(const std::vector<u8>& f) {
    NpdmResult r;
    if (f.size() < 0x80 || Get<u32>(f, 0) != META_MAGIC) { r.error = "main.npdm no valido (falta \"META\")"; return r; }
    NpdmInfo& i = r.info;
    const u8 flags = Get<u8>(f, 0x0C);
    i.is_64bit = flags & 1;
    i.address_space = (flags >> 1) & 7;
    i.main_priority = Get<u8>(f, 0x0E);
    i.main_core = Get<u8>(f, 0x0F);
    i.system_resource_size = Get<u32>(f, 0x14);
    i.main_stack_size = Get<u32>(f, 0x1C);
    for (size_t k = 0; k < 0x10 && f[0x20 + k]; ++k) i.name += char(f[0x20 + k]);

    const u32 aci_off = Get<u32>(f, 0x70), aci_size = Get<u32>(f, 0x74);
    if (aci_size >= 0x18 && u64(aci_off) + aci_size <= f.size() && Get<u32>(f, aci_off) == ACI0_MAGIC)
        i.program_id = Get<u64>(f, aci_off + 0x10);

    if (!i.is_64bit) { r.error = "programa de 32 bits: NeXo solo ejecuta programas de 64 bits"; return r; }
    if (i.main_priority > 63) i.main_priority = 44;
    if (i.main_stack_size < 0x4000) i.main_stack_size = 0x100000;
    i.main_stack_size = (i.main_stack_size + 0xFFF) & ~0xFFFu;
    r.ok = true;
    return r;
}

} // namespace NeXo2::Loader
