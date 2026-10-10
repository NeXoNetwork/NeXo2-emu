#pragma once
// Red del PC para los servicios de red emulados (bsd, sfdnsres, ssl).
//
// Esta cabecera no incluye nada de Windows/POSIX: los servicios solo ven numeros.
// Todo lo que devuelve un error lo hace con numeros de errno de LINUX, que son los que
// usa el servicio bsd de la consola (Nintendo compilo su FreeBSD con los errno de Linux)
// y los que libnx espera recibir.
//
// Solo IPv4 (como getaddrinfo de libnx, que pide AF_INET).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NeXo2::HLE::Net {

using Handle = std::intptr_t;
constexpr Handle INVALID_HANDLE = -1;

// errno de Linux (los que entiende libnx)
namespace Errno {
    constexpr int INTR = 4, BADF = 9, AGAIN = 11, NOMEM = 12, FAULT = 14, INVAL = 22, MFILE = 24,
                  PIPE = 32, NOTSOCK = 88, DESTADDRREQ = 89, MSGSIZE = 90, PROTOTYPE = 91,
                  NOPROTOOPT = 92, PROTONOSUPPORT = 93, OPNOTSUPP = 95, AFNOSUPPORT = 97,
                  ADDRINUSE = 98, ADDRNOTAVAIL = 99, NETDOWN = 100, NETUNREACH = 101,
                  CONNABORTED = 103, CONNRESET = 104, NOBUFS = 105, ISCONN = 106, NOTCONN = 107,
                  TIMEDOUT = 110, CONNREFUSED = 111, HOSTUNREACH = 113, ALREADY = 114,
                  INPROGRESS = 115;
}

// Direccion IPv4 tal como va en memoria (orden de red): puerto y IP en bytes "big endian"
struct Addr4 {
    std::uint8_t port[2] = {0, 0};
    std::uint8_t ip[4] = {0, 0, 0, 0};
};

// Arranca la red del PC (en Windows, WSAStartup). Se puede llamar muchas veces.
bool Init();

// Todas las funciones dejan el socket del PC en modo no bloqueante: las esperas las hace
// quien llama (con Poll), para no bloquear el emulador entero.
// Devuelven < 0 (o INVALID_HANDLE) con 'err' = errno de Linux si fallan.
Handle Socket(int type, int protocol, int& err);          // type: 1 = TCP, 2 = UDP
void   Close(Handle h);
int    Connect(Handle h, const Addr4& to, int& err);      // no bloqueante: INPROGRESS es normal
int    Bind(Handle h, const Addr4& addr, int& err);
int    Listen(Handle h, int backlog, int& err);
Handle Accept(Handle h, Addr4& from, int& err);
long   Send(Handle h, const void* data, std::size_t size, bool peek_unused, int& err);
long   Recv(Handle h, void* data, std::size_t size, bool peek, int& err);
long   SendTo(Handle h, const void* data, std::size_t size, const Addr4& to, int& err);
long   RecvFrom(Handle h, void* data, std::size_t size, bool peek, Addr4& from, int& err);
int    GetSockName(Handle h, Addr4& out, int& err);
int    GetPeerName(Handle h, Addr4& out, int& err);
int    Shutdown(Handle h, int how, int& err);             // 0 = lectura, 1 = escritura, 2 = las dos
int    PendingError(Handle h);                            // SO_ERROR (errno de Linux, 0 = nada)

// Opciones con los numeros de la consola (FreeBSD): nivel 0xFFFF = SOL_SOCKET, 6 = TCP
int SetSockOpt(Handle h, int level, int name, const void* value, std::size_t size, int& err);
int GetSockOpt(Handle h, int level, int name, void* value, std::size_t& size, int& err);

// Poll con los bits de la consola: 1 = POLLIN, 2 = POLLPRI, 4 = POLLOUT, 8 = POLLERR,
// 0x10 = POLLHUP, 0x20 = POLLNVAL. timeout_ms < 0 = sin limite.
struct PollEntry {
    Handle handle = INVALID_HANDLE;
    short events = 0;
    short revents = 0;
};
int Poll(std::vector<PollEntry>& entries, int timeout_ms, int& err);

// DNS: direcciones IPv4 de 'host'. Devuelve 0 o un codigo EAI_* de la consola
// (2 = EAI_AGAIN, 4 = EAI_FAIL, 8 = EAI_NONAME).
int Resolve(const std::string& host, std::vector<Addr4>& out);

// IP del PC en la red local (para nifm). 127.0.0.1 si no hay red.
Addr4 LocalAddress();

} // namespace NeXo2::HLE::Net
