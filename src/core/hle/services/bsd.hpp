#pragma once
// Red emulada con la red del PC:
//   bsd:u / bsd:s   sockets (TCP/UDP IPv4) -> sockets del PC (net/host_net.hpp)
//   sfdnsres        DNS (getaddrinfo de libnx) -> DNS del PC
//   nifm:u          estado de la red: conectada (wifi, senal maxima)
// Referencia del formato: libnx (services/bsd.c, sfdnsres.c, runtime/resolver.c).
// Ver docs/07-nexo-internals/network.md.
#include <map>
#include "hle/service.hpp"
#include "hle/net/host_net.hpp"

namespace NeXo2::HLE {

// Un socket del programa
struct GuestSocket {
    Net::Handle handle = Net::INVALID_HANDLE;
    int  type = 1;                 // 1 = TCP, 2 = UDP
    bool nonblocking = false;      // O_NONBLOCK del programa (el del PC siempre es no bloqueante)
    int  recv_timeout_ms = -1;     // SO_RCVTIMEO / SO_SNDTIMEO (-1 = sin limite)
    int  send_timeout_ms = -1;
};

// Sockets del proceso (compartidos por todas las sesiones de bsd y por ssl)
class NetworkState {
public:
    ~NetworkState();
    s32 Add(const GuestSocket& s);
    GuestSocket* Get(s32 fd);
    void Remove(s32 fd);   // cierra el socket del PC
private:
    std::map<s32, GuestSocket> m_sockets;
    s32 m_next = 3;
};

class BsdService final : public ServiceObject {
public:
    explicit BsdService(std::string name);
};

class SfdnsresService final : public ServiceObject {
public:
    SfdnsresService();
};

class NifmService final : public ServiceObject {
public:
    explicit NifmService(std::string name);
};
class NifmGeneralService final : public ServiceObject {
public:
    NifmGeneralService();
};

// Espera a que el socket del PC este listo ('events' con los bits de la consola: 1 leer,
// 4 escribir) soltando el candado del kernel, para que los demas nucleos sigan.
// Devuelve los eventos (0 = se acabo el tiempo, -1 = el emulador se esta parando).
int WaitSocket(class Kernel& kernel, Net::Handle h, short events, int timeout_ms);

} // namespace NeXo2::HLE
