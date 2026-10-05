#pragma once
#include "hle/service.hpp"

namespace NeXo2::HLE {

// "sm:" (Service Manager): la puerta de entrada a todos los servicios.
// El programa se conecta con svcConnectToNamedPort("sm:") y le pide handles
// de otros servicios por nombre (GetServiceHandle).
// Referencia: https://switchbrew.org/wiki/Services_API
class ServiceManager final : public ServiceObject {
public:
    ServiceManager();
    bool SupportsTipc() const override { return true; }

private:
    void RegisterClient(IpcContext& ctx);
    void GetServiceHandle(IpcContext& ctx);
    void DetachClient(IpcContext& ctx);
};

} // namespace NeXo2::HLE
