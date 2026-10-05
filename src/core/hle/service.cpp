#include "service.hpp"
#include "common/logger.hpp"

namespace NeXo2::HLE {

using Common::Logger;

void ServiceObject::RegisterCommand(u32 id, const char* name, Handler handler) {
    m_commands[id] = Command{name, std::move(handler)};
}

void ServiceObject::RegisterStub(u32 id, const char* name) {
    m_commands[id] = Command{std::string(name) + " [stub]", [](IpcContext& ctx) { ctx.SetResult(0); }};
}

std::string ServiceObject::CommandName(u32 id) const {
    auto it = m_commands.find(id);
    return it == m_commands.end() ? std::string() : it->second.name;
}

void ServiceObject::Dispatch(IpcContext& ctx) {
    auto it = m_commands.find(ctx.CommandId());
    if (it == m_commands.end()) {
        ctx.Unimplemented(m_name);
        return;
    }
    Logger::Log(Logger::Level::Info, "[IPC] " + m_name + " -> " + it->second.name +
                                     " (" + std::to_string(ctx.CommandId()) + ")");
    it->second.handler(ctx);
}

std::shared_ptr<ServiceObject> ServiceRegistry::Create(const std::string& name) const {
    auto it = m_factories.find(name);
    if (it == m_factories.end()) {
        Logger::Log(Logger::Level::Warning, "[HLE] El programa pide el servicio '" + name +
                                            "', que todavia no existe en NeXo");
        return std::make_shared<UnimplementedService>(name);
    }
    return it->second();
}

} // namespace NeXo2::HLE
