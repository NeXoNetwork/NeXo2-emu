#pragma once
#include "hle/service.hpp"

namespace NeXo2::HLE {

// "apm": modos de rendimiento (frecuencias de CPU/GPU). libnx lo abre al
// arrancar una aplicacion. Referencia: https://switchbrew.org/wiki/PPC_services#apm
class ApmManager final : public ServiceObject {
public:
    ApmManager();
};

// ISession de apm (lo devuelve OpenSession)
class ApmSession final : public ServiceObject {
public:
    ApmSession();
};

} // namespace NeXo2::HLE
