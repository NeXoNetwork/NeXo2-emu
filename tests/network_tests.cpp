// Tests de la red del PC que usan los servicios bsd/sfdnsres/ssl (net/host_net.hpp):
// un servidor y un cliente TCP en 127.0.0.1, sin salir del PC.
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"
#include "hle/net/host_net.hpp"

using namespace NeXo2;
namespace Net = NeXo2::HLE::Net;

namespace {
// Espera (como mucho 2 s) a que el socket este listo
bool WaitReady(Net::Handle h, short events) {
    std::vector<Net::PollEntry> e{{h, events, 0}};
    int err = 0;
    return Net::Poll(e, 2000, err) > 0 && (e[0].revents & events);
}
}

TEST(Network_LoopbackTcp) {
    CHECK(Net::Init());
    int err = 0;
    // Servidor en un puerto libre de 127.0.0.1
    const Net::Handle server = Net::Socket(1, 0, err);
    CHECK(server != Net::INVALID_HANDLE);
    Net::Addr4 any;
    any.ip[0] = 127; any.ip[3] = 1;
    CHECK_EQ(Net::Bind(server, any, err), 0);
    CHECK_EQ(Net::Listen(server, 4, err), 0);
    Net::Addr4 bound;
    CHECK_EQ(Net::GetSockName(server, bound, err), 0);
    CHECK(bound.port[0] != 0 || bound.port[1] != 0);

    // Cliente: conectar es no bloqueante (INPROGRESS o 0), luego esperar a poder escribir
    const Net::Handle client = Net::Socket(1, 0, err);
    const int rc = Net::Connect(client, bound, err);
    CHECK(rc == 0 || err == Net::Errno::INPROGRESS);
    CHECK(WaitReady(server, 1));
    Net::Addr4 from;
    const Net::Handle conn = Net::Accept(server, from, err);
    CHECK(conn != Net::INVALID_HANDLE);
    CHECK(WaitReady(client, 4));
    CHECK_EQ(Net::PendingError(client), 0);

    // Sin datos: leer da EAGAIN (los sockets del PC son no bloqueantes)
    char buf[64] = {};
    CHECK_EQ(Net::Recv(conn, buf, sizeof(buf), false, err), -1L);
    CHECK_EQ(err, Net::Errno::AGAIN);

    const std::string msg = "hola NeXo";
    CHECK_EQ(Net::Send(client, msg.data(), msg.size(), false, err), long(msg.size()));
    CHECK(WaitReady(conn, 1));
    CHECK_EQ(Net::Recv(conn, buf, sizeof(buf), true, err), long(msg.size()));    // peek
    CHECK_EQ(Net::Recv(conn, buf, sizeof(buf), false, err), long(msg.size()));
    CHECK(std::string(buf, msg.size()) == msg);

    // Opciones con numeros de la consola: TCP_NODELAY (6, 1) y SO_TYPE (0xFFFF, 0x1008)
    const int one = 1;
    CHECK_EQ(Net::SetSockOpt(client, 6, 1, &one, sizeof(one), err), 0);
    int type = 0;
    size_t size = sizeof(type);
    CHECK_EQ(Net::GetSockOpt(client, 0xFFFF, 0x1008, &type, size, err), 0);
    CHECK_EQ(type, 1);   // SOCK_STREAM

    // El cliente cierra: el servidor lee 0 (fin)
    Net::Close(client);
    CHECK(WaitReady(conn, 1));
    CHECK_EQ(Net::Recv(conn, buf, sizeof(buf), false, err), 0L);
    Net::Close(conn);
    Net::Close(server);
}

TEST(Network_ResolveLocalhost) {
    std::vector<Net::Addr4> addrs;
    CHECK_EQ(Net::Resolve("localhost", addrs), 0);
    CHECK(!addrs.empty());
    if (!addrs.empty()) CHECK_EQ(addrs[0].ip[0], 127);
    addrs.clear();
    CHECK(Net::Resolve("no-existe.invalid", addrs) != 0);
}
