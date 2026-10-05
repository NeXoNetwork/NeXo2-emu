#include "sm.hpp"
#include "hle/kernel.hpp"
#include "common/logger.hpp"

namespace NeXo2::HLE {

ServiceManager::ServiceManager() : ServiceObject("sm:") {
    RegisterCommand(0, "RegisterClient",   [this](IpcContext& ctx) { RegisterClient(ctx); });
    RegisterCommand(1, "GetServiceHandle", [this](IpcContext& ctx) { GetServiceHandle(ctx); });
    RegisterCommand(4, "DetachClient",     [this](IpcContext& ctx) { DetachClient(ctx); });
}

// RegisterClient(pid): el programa se presenta. Solo hay un proceso, asi que vale siempre.
void ServiceManager::RegisterClient(IpcContext& ctx) {
    ctx.SetResult(Result::Success);
}

// GetServiceHandle(nombre: u64) -> handle de una sesion nueva con ese servicio
void ServiceManager::GetServiceHandle(IpcContext& ctx) {
    const std::string name = ServiceNameFromU64(ctx.Pop<u64>());
    if (name.empty()) {
        ctx.SetResult(0x1015); // sm: InvalidServiceName (modulo 21, descripcion 8)
        return;
    }
    Kernel& kernel = ctx.GetKernel();
    const u32 handle = kernel.CreateSessionHandle(kernel.Services().Create(name));
    Common::Logger::Log(Common::Logger::Level::Info,
                        "[sm:] GetServiceHandle(\"" + name + "\") -> handle " + std::to_string(handle));
    ctx.PushMoveHandle(handle);
    ctx.SetResult(Result::Success);
}

void ServiceManager::DetachClient(IpcContext& ctx) {
    ctx.SetResult(Result::Success);
}

} // namespace NeXo2::HLE
