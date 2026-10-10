// Red del PC: sockets de Windows (Winsock) o POSIX. Ver host_net.hpp.
#include "host_net.hpp"
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SockLen = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
#endif

namespace NeXo2::HLE::Net {

namespace {

#ifdef _WIN32
using Native = SOCKET;
inline Native N(Handle h) { return static_cast<Native>(h); }
inline bool Bad(Native s) { return s == INVALID_SOCKET; }

int LastError() {
    switch (WSAGetLastError()) {
        case WSAEWOULDBLOCK:     return Errno::AGAIN;
        case WSAEINPROGRESS:     return Errno::INPROGRESS;
        case WSAEALREADY:        return Errno::ALREADY;
        case WSAENOTSOCK:        return Errno::NOTSOCK;
        case WSAEDESTADDRREQ:    return Errno::DESTADDRREQ;
        case WSAEMSGSIZE:        return Errno::MSGSIZE;
        case WSAEPROTOTYPE:      return Errno::PROTOTYPE;
        case WSAENOPROTOOPT:     return Errno::NOPROTOOPT;
        case WSAEPROTONOSUPPORT: return Errno::PROTONOSUPPORT;
        case WSAEOPNOTSUPP:      return Errno::OPNOTSUPP;
        case WSAEAFNOSUPPORT:    return Errno::AFNOSUPPORT;
        case WSAEADDRINUSE:      return Errno::ADDRINUSE;
        case WSAEADDRNOTAVAIL:   return Errno::ADDRNOTAVAIL;
        case WSAENETDOWN:        return Errno::NETDOWN;
        case WSAENETUNREACH:     return Errno::NETUNREACH;
        case WSAECONNABORTED:    return Errno::CONNABORTED;
        case WSAECONNRESET:      return Errno::CONNRESET;
        case WSAENOBUFS:         return Errno::NOBUFS;
        case WSAEISCONN:         return Errno::ISCONN;
        case WSAENOTCONN:        return Errno::NOTCONN;
        case WSAESHUTDOWN:       return Errno::PIPE;
        case WSAETIMEDOUT:       return Errno::TIMEDOUT;
        case WSAECONNREFUSED:    return Errno::CONNREFUSED;
        case WSAEHOSTUNREACH:    return Errno::HOSTUNREACH;
        case WSAEMFILE:          return Errno::MFILE;
        case WSAEBADF:           return Errno::BADF;
        case WSAEINTR:           return Errno::INTR;
        case WSAEFAULT:          return Errno::FAULT;
        case WSAEINVAL:          return Errno::INVAL;
        default:                 return Errno::NETDOWN;
    }
}
int ErrorFromCode(int code) { WSASetLastError(code); return LastError(); }
#else
using Native = int;
inline Native N(Handle h) { return static_cast<Native>(h); }
inline bool Bad(Native s) { return s < 0; }

int Translate(int e) {
    switch (e) {
        case EAGAIN:          return Errno::AGAIN;
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:     return Errno::AGAIN;
#endif
        case EINPROGRESS:     return Errno::INPROGRESS;
        case EALREADY:        return Errno::ALREADY;
        case ENOTSOCK:        return Errno::NOTSOCK;
        case EDESTADDRREQ:    return Errno::DESTADDRREQ;
        case EMSGSIZE:        return Errno::MSGSIZE;
        case EPROTOTYPE:      return Errno::PROTOTYPE;
        case ENOPROTOOPT:     return Errno::NOPROTOOPT;
        case EPROTONOSUPPORT: return Errno::PROTONOSUPPORT;
        case EOPNOTSUPP:      return Errno::OPNOTSUPP;
        case EAFNOSUPPORT:    return Errno::AFNOSUPPORT;
        case EADDRINUSE:      return Errno::ADDRINUSE;
        case EADDRNOTAVAIL:   return Errno::ADDRNOTAVAIL;
        case ENETDOWN:        return Errno::NETDOWN;
        case ENETUNREACH:     return Errno::NETUNREACH;
        case ECONNABORTED:    return Errno::CONNABORTED;
        case ECONNRESET:      return Errno::CONNRESET;
        case ENOBUFS:         return Errno::NOBUFS;
        case EISCONN:         return Errno::ISCONN;
        case ENOTCONN:        return Errno::NOTCONN;
        case EPIPE:           return Errno::PIPE;
        case ETIMEDOUT:       return Errno::TIMEDOUT;
        case ECONNREFUSED:    return Errno::CONNREFUSED;
        case EHOSTUNREACH:    return Errno::HOSTUNREACH;
        case EMFILE:          return Errno::MFILE;
        case EBADF:           return Errno::BADF;
        case EINTR:           return Errno::INTR;
        case EFAULT:          return Errno::FAULT;
        case EINVAL:          return Errno::INVAL;
        case ENOMEM:          return Errno::NOMEM;
        default:              return Errno::NETDOWN;
    }
}
int LastError() { return Translate(errno); }
int ErrorFromCode(int code) { return Translate(code); }
#endif

void SetNonBlocking(Native s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

sockaddr_in ToNative(const Addr4& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    std::memcpy(&sa.sin_port, a.port, 2);
    std::memcpy(&sa.sin_addr, a.ip, 4);
    return sa;
}
Addr4 FromNative(const sockaddr_in& sa) {
    Addr4 a;
    std::memcpy(a.port, &sa.sin_port, 2);
    std::memcpy(a.ip, &sa.sin_addr, 4);
    return a;
}

#ifdef _WIN32
constexpr int SEND_FLAGS = 0;
#else
constexpr int SEND_FLAGS = MSG_NOSIGNAL;   // un socket cerrado no debe matar al emulador (SIGPIPE)
#endif

} // namespace

bool Init() {
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
#else
    return true;
#endif
}

Handle Socket(int type, int protocol, int& err) {
    Init();
    const int native_type = type == 2 ? SOCK_DGRAM : type == 3 ? SOCK_RAW : SOCK_STREAM;
    Native s = ::socket(AF_INET, native_type, protocol);
    if (Bad(s)) { err = LastError(); return INVALID_HANDLE; }
    SetNonBlocking(s);
    return static_cast<Handle>(s);
}

void Close(Handle h) {
    if (h == INVALID_HANDLE) return;
#ifdef _WIN32
    ::closesocket(N(h));
#else
    ::close(N(h));
#endif
}

int Connect(Handle h, const Addr4& to, int& err) {
    const sockaddr_in sa = ToNative(to);
    if (::connect(N(h), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0) return 0;
    err = LastError();
    if (err == Errno::AGAIN) err = Errno::INPROGRESS;   // Windows dice "would block"
    return -1;
}

int Bind(Handle h, const Addr4& addr, int& err) {
    const sockaddr_in sa = ToNative(addr);
    if (::bind(N(h), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0) return 0;
    err = LastError();
    return -1;
}

int Listen(Handle h, int backlog, int& err) {
    if (::listen(N(h), backlog) == 0) return 0;
    err = LastError();
    return -1;
}

Handle Accept(Handle h, Addr4& from, int& err) {
    sockaddr_in sa{};
    SockLen len = sizeof(sa);
    Native s = ::accept(N(h), reinterpret_cast<sockaddr*>(&sa), &len);
    if (Bad(s)) { err = LastError(); return INVALID_HANDLE; }
    SetNonBlocking(s);
    from = FromNative(sa);
    return static_cast<Handle>(s);
}

long Send(Handle h, const void* data, std::size_t size, bool, int& err) {
    const auto n = ::send(N(h), static_cast<const char*>(data), static_cast<int>(size), SEND_FLAGS);
    if (n < 0) { err = LastError(); return -1; }
    return static_cast<long>(n);
}

long Recv(Handle h, void* data, std::size_t size, bool peek, int& err) {
    const auto n = ::recv(N(h), static_cast<char*>(data), static_cast<int>(size), peek ? MSG_PEEK : 0);
    if (n < 0) { err = LastError(); return -1; }
    return static_cast<long>(n);
}

long SendTo(Handle h, const void* data, std::size_t size, const Addr4& to, int& err) {
    const sockaddr_in sa = ToNative(to);
    const auto n = ::sendto(N(h), static_cast<const char*>(data), static_cast<int>(size), SEND_FLAGS,
                            reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    if (n < 0) { err = LastError(); return -1; }
    return static_cast<long>(n);
}

long RecvFrom(Handle h, void* data, std::size_t size, bool peek, Addr4& from, int& err) {
    sockaddr_in sa{};
    SockLen len = sizeof(sa);
    const auto n = ::recvfrom(N(h), static_cast<char*>(data), static_cast<int>(size), peek ? MSG_PEEK : 0,
                              reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0) { err = LastError(); return -1; }
    from = FromNative(sa);
    return static_cast<long>(n);
}

int GetSockName(Handle h, Addr4& out, int& err) {
    sockaddr_in sa{};
    SockLen len = sizeof(sa);
    if (::getsockname(N(h), reinterpret_cast<sockaddr*>(&sa), &len) != 0) { err = LastError(); return -1; }
    out = FromNative(sa);
    return 0;
}

int GetPeerName(Handle h, Addr4& out, int& err) {
    sockaddr_in sa{};
    SockLen len = sizeof(sa);
    if (::getpeername(N(h), reinterpret_cast<sockaddr*>(&sa), &len) != 0) { err = LastError(); return -1; }
    out = FromNative(sa);
    return 0;
}

int Shutdown(Handle h, int how, int& err) {
#ifdef _WIN32
    const int native = how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH;
#else
    const int native = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR;
#endif
    if (::shutdown(N(h), native) == 0) return 0;
    err = LastError();
    return -1;
}

int PendingError(Handle h) {
    int value = 0;
    SockLen len = sizeof(value);
    if (::getsockopt(N(h), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &len) != 0) return LastError();
    return value ? ErrorFromCode(value) : 0;
}

// ----------------------------------------------------------------------------
//  Opciones: numeros de FreeBSD (consola) -> los del PC
// ----------------------------------------------------------------------------
namespace {
constexpr int BSD_SOL_SOCKET = 0xFFFF;
bool NativeOption(int level, int name, int& nlevel, int& nname) {
    if (level == BSD_SOL_SOCKET) {
        nlevel = SOL_SOCKET;
        switch (name) {
            case 0x0004: nname = SO_REUSEADDR; return true;
            case 0x0008: nname = SO_KEEPALIVE; return true;
            case 0x0020: nname = SO_BROADCAST; return true;
            case 0x0080: nname = SO_LINGER;    return true;
            case 0x0100: nname = SO_OOBINLINE; return true;
            case 0x1001: nname = SO_SNDBUF;    return true;
            case 0x1002: nname = SO_RCVBUF;    return true;
            case 0x1007: nname = SO_ERROR;     return true;
            case 0x1008: nname = SO_TYPE;      return true;
            default: return false;
        }
    }
    if (level == 6) {   // IPPROTO_TCP
        nlevel = IPPROTO_TCP;
        if (name == 1) { nname = TCP_NODELAY; return true; }
        return false;
    }
    if (level == 0) {   // IPPROTO_IP
        nlevel = IPPROTO_IP;
        if (name == 4) { nname = IP_TTL; return true; }
        return false;
    }
    return false;
}
} // namespace

int SetSockOpt(Handle h, int level, int name, const void* value, std::size_t size, int& err) {
    int nlevel = 0, nname = 0;
    if (!NativeOption(level, name, nlevel, nname)) return 0;   // opcion sin equivalente: se acepta y se ignora
    if (::setsockopt(N(h), nlevel, nname, static_cast<const char*>(value), static_cast<SockLen>(size)) == 0) return 0;
    err = LastError();
    return -1;
}

int GetSockOpt(Handle h, int level, int name, void* value, std::size_t& size, int& err) {
    int nlevel = 0, nname = 0;
    if (!NativeOption(level, name, nlevel, nname)) {
        std::memset(value, 0, size);
        return 0;
    }
    if (nlevel == SOL_SOCKET && nname == SO_ERROR && size >= sizeof(int)) {
        const int e = PendingError(h);
        std::memcpy(value, &e, sizeof(int));
        size = sizeof(int);
        return 0;
    }
    SockLen len = static_cast<SockLen>(size);
    if (::getsockopt(N(h), nlevel, nname, static_cast<char*>(value), &len) != 0) { err = LastError(); return -1; }
    size = static_cast<std::size_t>(len);
    return 0;
}

// ----------------------------------------------------------------------------
//  Poll
// ----------------------------------------------------------------------------
int Poll(std::vector<PollEntry>& entries, int timeout_ms, int& err) {
    // (con B_: en Windows IN y OUT son macros vacias)
    constexpr short B_IN = 1, B_PRI = 2, B_OUT = 4, B_ERR = 8, B_HUP = 0x10, B_NVAL = 0x20;
#ifdef _WIN32
    std::vector<WSAPOLLFD> fds;
#else
    std::vector<pollfd> fds;
#endif
    std::vector<size_t> index;
    int invalid = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].revents = 0;
        if (entries[i].handle == INVALID_HANDLE) {
            entries[i].revents = B_NVAL;
            ++invalid;
            continue;
        }
        short ev = 0;
        if (entries[i].events & (B_IN | B_PRI)) ev |= POLLIN;
        if (entries[i].events & B_OUT) ev |= POLLOUT;
        fds.push_back({N(entries[i].handle), ev, 0});
        index.push_back(i);
    }
    if (fds.empty()) return invalid;
#ifdef _WIN32
    const int n = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
#else
    const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
#endif
    if (n < 0) { err = LastError(); return -1; }
    int ready = invalid;
    for (size_t k = 0; k < fds.size(); ++k) {
        const short r = fds[k].revents;
        short out = 0;
        if (r & POLLIN) out |= B_IN;
        if (r & POLLOUT) out |= B_OUT;
        if (r & POLLERR) out |= B_ERR;
        if (r & POLLHUP) out |= B_HUP | (entries[index[k]].events & B_IN);   // cerrado: leer devuelve 0
        if (r & POLLNVAL) out |= B_NVAL;
        entries[index[k]].revents = out;
        if (out) ++ready;
    }
    return ready;
}

// ----------------------------------------------------------------------------
//  DNS y direccion local
// ----------------------------------------------------------------------------
int Resolve(const std::string& host, std::vector<Addr4>& out) {
    Init();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0) {
        if (rc == EAI_AGAIN) return 2;
        if (rc == EAI_NONAME) return 8;
        return 4;
    }
    for (addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family != AF_INET || !p->ai_addr) continue;
        const Addr4 a = FromNative(*reinterpret_cast<const sockaddr_in*>(p->ai_addr));
        bool dup = false;
        for (const auto& e : out) dup |= std::memcmp(e.ip, a.ip, 4) == 0;
        if (!dup) out.push_back(a);
    }
    ::freeaddrinfo(res);
    return out.empty() ? 8 : 0;
}

Addr4 LocalAddress() {
    Addr4 local;
    local.ip[0] = 127; local.ip[3] = 1;
    int err = 0;
    const Handle h = Socket(2, 0, err);
    if (h == INVALID_HANDLE) return local;
    Addr4 dns;
    dns.ip[0] = 8; dns.ip[1] = 8; dns.ip[2] = 8; dns.ip[3] = 8;
    dns.port[1] = 53;
    // "Conectar" un socket UDP no envia nada: solo elige la interfaz de salida
    Addr4 mine;
    if (Connect(h, dns, err) == 0 && GetSockName(h, mine, err) == 0 && mine.ip[0] != 0) local = mine;
    Close(h);
    return local;
}

} // namespace NeXo2::HLE::Net
