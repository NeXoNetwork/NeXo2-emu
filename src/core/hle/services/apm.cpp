#include "apm.hpp"
#include "hle/kernel.hpp"

namespace NeXo2::HLE {

// Configuracion de rendimiento "normal" (ver switchbrew: 0x00010000 = CPU 1020 MHz...)
constexpr u32 DEFAULT_PERFORMANCE_CONFIGURATION = 0x00010000;

ApmManager::ApmManager() : ServiceObject("apm") {
    RegisterCommand(0, "OpenSession", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<ApmSession>());
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "GetPerformanceMode", [](IpcContext& ctx) {
        ctx.Push<u32>(0); // 0 = Normal (portatil)
        ctx.SetResult(Result::Success);
    });
}

ApmSession::ApmSession() : ServiceObject("apm:ISession") {
    RegisterStub(0, "SetPerformanceConfiguration");
    RegisterCommand(1, "GetPerformanceConfiguration", [](IpcContext& ctx) {
        ctx.Push<u32>(DEFAULT_PERFORMANCE_CONFIGURATION);
        ctx.SetResult(Result::Success);
    });
}

} // namespace NeXo2::HLE
