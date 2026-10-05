#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include "common/types.hpp"
#include "ipc.hpp"

// Base de todos los servicios HLE (sm:, set:sys, fsp-srv...).
//
// Un servicio es una lista de comandos numerados. Cada comando es una funcion
// C++ que lee sus argumentos del IpcContext y escribe la respuesta.
//
//   class MiServicio : public ServiceObject {
//   public:
//       MiServicio() : ServiceObject("mi:srv") {
//           RegisterCommand(0, "Saludar", [this](IpcContext& ctx) { Saludar(ctx); });
//       }
//   };
//
// Si el programa llama a un comando que no esta en la lista, la CPU se para
// con un mensaje "Servicio 'mi:srv': comando N no implementado".
namespace NeXo2::HLE {

class ServiceObject : public std::enable_shared_from_this<ServiceObject> {
public:
    explicit ServiceObject(std::string name) : m_name(std::move(name)) {}
    virtual ~ServiceObject() = default;

    const std::string& Name() const { return m_name; }

    // Busca el comando y lo ejecuta (o informa de que falta).
    virtual void Dispatch(IpcContext& ctx);

    // TIPC: por defecto usa la misma tabla de comandos que CMIF.
    virtual bool SupportsTipc() const { return false; }

    // Nombre de un comando registrado ("" si no existe)
    std::string CommandName(u32 id) const;

    using Handler = std::function<void(IpcContext&)>;

protected:
    void RegisterCommand(u32 id, const char* name, Handler handler);

    // Comando que solo tiene que responder "OK" (ignora sus argumentos).
    // Util para ajustes que todavia no afectan a nada en NeXo.
    void RegisterStub(u32 id, const char* name);

private:
    struct Command { std::string name; Handler handler; };
    std::string m_name;
    std::map<u32, Command> m_commands;
};

// Servicio que todavia no existe en NeXo: cualquier comando para la CPU.
// sm: lo entrega cuando el programa pide un servicio que no conocemos, asi el
// mensaje de error dice exactamente que servicio y que comando hace falta.
class UnimplementedService final : public ServiceObject {
public:
    explicit UnimplementedService(std::string name) : ServiceObject(std::move(name)) {}
    void Dispatch(IpcContext& ctx) override { ctx.Unimplemented(Name()); }
};

// Catalogo de servicios con nombre (lo que sm: puede entregar)
class ServiceRegistry {
public:
    using Factory = std::function<std::shared_ptr<ServiceObject>()>;

    void Register(const std::string& name, Factory factory) { m_factories[name] = std::move(factory); }
    bool Has(const std::string& name) const { return m_factories.count(name) != 0; }

    // Crea una instancia nueva del servicio (o un UnimplementedService si no existe)
    std::shared_ptr<ServiceObject> Create(const std::string& name) const;

private:
    std::map<std::string, Factory> m_factories;
};

} // namespace NeXo2::HLE
