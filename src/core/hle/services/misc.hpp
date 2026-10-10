#pragma once
#include "hle/service.hpp"

// Servicios pequenos que piden programas como el hbmenu: bateria (psm), red (nifm)...
// Contestan como una consola en modo portatil con la bateria llena y sin red.
namespace NeXo2::HLE {

// "psm": bateria y cargador. Referencia: https://switchbrew.org/wiki/PTM_services#psm
class PsmService final : public ServiceObject {
public:
    PsmService();
};

// IPsmSession (OpenSession): avisos de cambios de bateria
class PsmSession final : public ServiceObject {
public:
    PsmSession();
};

// "bsd:u" / "bsd:s": sockets. NeXo aun no tiene red: el registro funciona (libnx arranca sus
// sockets sin error) y cada operacion contesta -1 con errno ENETDOWN ("la red no esta").
class BsdService final : public ServiceObject {
public:
    explicit BsdService(std::string name);
};

// "nifm:u": estado de la red. Siempre "sin conexion".
class NifmService final : public ServiceObject {
public:
    explicit NifmService(std::string name);
};
class NifmGeneralService final : public ServiceObject {
public:
    NifmGeneralService();
};

// "ts": temperatura (el hbmenu la ensena arriba). Siempre 35 grados.
class TsService final : public ServiceObject {
public:
    TsService();
};
class TsSession final : public ServiceObject {
public:
    TsSession();
};

// "pl:u": fuentes compartidas del sistema (las usa el hbmenu para escribir). No tenemos
// las de Nintendo: se usa una fuente TrueType del PC (NEXO2_FONT=ruta.ttf, o una del sistema:
// Segoe UI / Arial en Windows, DejaVu en Linux) para todos los tipos de fuente.
class PlService final : public ServiceObject {
public:
    PlService();
};

} // namespace NeXo2::HLE
