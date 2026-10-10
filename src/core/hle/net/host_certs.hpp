#pragma once
// Certificados raiz del PC, para comprobar los servidores https (servicio ssl).
// Windows: el almacen "ROOT" del sistema. Linux: el paquete de certificados de la distro.
// NEXO2_CA_FILE=archivo.pem anade los de ese archivo.
#include <cstdint>
#include <vector>

namespace NeXo2::HLE::Net {

struct CertBlob {
    std::vector<std::uint8_t> data;
    bool pem = false;   // true: texto PEM (con '\0' al final); false: un certificado DER
};

// Se leen una vez y se guardan
const std::vector<CertBlob>& SystemRootCerts();

} // namespace NeXo2::HLE::Net
