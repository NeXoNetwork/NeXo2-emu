#include "service.hpp"
#include <cstdlib>
#include <cstdio>
#include <set>
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
    // Cada comando se apunta en el registro la primera vez (si no, NZ:P escribe miles de
    // lineas por segundo y eso frena la emulacion). NEXO2_LOG_IPC=1: todas las llamadas.
    static const bool all = std::getenv("NEXO2_LOG_IPC") != nullptr;
    if (all || !it->second.logged) {
        it->second.logged = true;
        Logger::Log(Logger::Level::Info, "[IPC] " + m_name + " -> " + it->second.name +
                                         " (" + std::to_string(ctx.CommandId()) + ")");
    }
    it->second.handler(ctx);
    // Errores devueltos al programa: cada (comando, error) una vez. Ayuda a ver por que un
    // juego se para justo despues ("no hay mensajes" y similares son normales).
    if (const u32 r = ctx.GetResult(); r != 0) {
        static std::set<std::string> seen;
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%X", r);
        if (seen.insert(m_name + ":" + std::to_string(ctx.CommandId()) + ":" + hex).second)
            Logger::Log(Logger::Level::Info, "[IPC] " + m_name + " -> " + it->second.name + " devuelve el error " + hex);
    }
}

void LenientService::Dispatch(IpcContext& ctx) {
    if (HasCommand(ctx.CommandId())) { ServiceObject::Dispatch(ctx); return; }
    static std::set<std::string> warned;
    const std::string key = Name() + ":" + std::to_string(ctx.CommandId());
    if (warned.insert(key).second)
        Logger::Log(Logger::Level::Warning, "[HLE] " + Name() + ": comando " + std::to_string(ctx.CommandId()) +
                                            " desconocido; se contesta OK sin datos");
    ctx.SetResult(0);
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
