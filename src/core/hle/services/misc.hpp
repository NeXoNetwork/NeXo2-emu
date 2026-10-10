#pragma once
#include "hle/service.hpp"
#include <map>
#include <string>

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

// "audren:u": renderizador de audio. Aun no hay audio: contesta "no disponible" y los
// programas (SDL) siguen sin sonido.
class AudioRendererManager final : public ServiceObject {
public:
    AudioRendererManager();
};

// "csrng": numeros aleatorios de calidad (claves, TLS de los programas)
class CsrngService final : public ServiceObject {
public:
    CsrngService();
};

} // namespace NeXo2::HLE
