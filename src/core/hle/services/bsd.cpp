#include "bsd.hpp"
#include "hle/kernel.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace NeXo2::HLE {

using Common::Logger;
namespace E = Net::Errno;

// ============================================================================
//  Sockets del proceso
// ============================================================================

NetworkState& Kernel::Network() {
    if (!m_network) {
        Net::Init();
        m_network = std::make_shared<NetworkState>();
    }
    return *m_network;
}

NetworkState::~NetworkState() {
    for (auto& [fd, s] : m_sockets) Net::Close(s.handle);
}

s32 NetworkState::Add(const GuestSocket& s) {
    while (m_sockets.count(m_next)) ++m_next;
    const s32 fd = m_next++;
    m_sockets[fd] = s;
    return fd;
}

GuestSocket* NetworkState::Get(s32 fd) {
    auto it = m_sockets.find(fd);
    return it == m_sockets.end() ? nullptr : &it->second;
}

void NetworkState::Remove(s32 fd) {
    auto it = m_sockets.find(fd);
    if (it == m_sockets.end()) return;
    Net::Close(it->second.handle);
    m_sockets.erase(it);
    if (fd < m_next) m_next = std::max<s32>(3, fd);
}

int WaitSocket(Kernel& kernel, Net::Handle h, short events, int timeout_ms) {
    using Clock = std::chrono::steady_clock;
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        // A trozos de 20 ms: asi se puede parar el emulador aunque el programa espere sin limite
        int slice = 20;
        if (timeout_ms >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            slice = int(std::clamp<long long>(left, 0, 20));
        }
        std::vector<Net::PollEntry> entries{{h, events, 0}};
        int err = 0;
        const bool released = kernel.ReleaseLockForWait();
        const int n = Net::Poll(entries, slice, err);
        kernel.ReacquireLockAfterWait(released);
        if (n < 0) return 8;                      // error: que la operacion lo descubra
        if (n > 0) return entries[0].revents ? entries[0].revents : events;
        if (kernel.StopRequested()) return -1;
        if (timeout_ms >= 0 && Clock::now() >= end) return 0;
    }
}

// ============================================================================
//  bsd:u / bsd:s
// ============================================================================
namespace {

constexpr int BSD_AF_INET = 2;
constexpr int BSD_SOCK_NONBLOCK = 0x20000000;
constexpr int BSD_SOCK_CLOEXEC = 0x10000000;
constexpr int BSD_MSG_PEEK = 0x2;
constexpr int BSD_MSG_DONTWAIT = 0x80;
constexpr int NX_O_NONBLOCK = 0x800;      // libnx pasa O_NONBLOCK con el valor de Linux
constexpr int BSD_F_GETFL = 3, BSD_F_SETFL = 4;
constexpr int BSD_SOL_SOCKET = 0xFFFF, BSD_SO_SNDTIMEO = 0x1005, BSD_SO_RCVTIMEO = 0x1006;
constexpr int BSD_SO_TYPE = 0x1008, BSD_SO_NOSIGPIPE = 0x0800;
constexpr short POLL_IN = 1, POLL_OUT = 4;

// Respuesta de bsd: s32 resultado, u32 errno (0 si fue bien)
void Reply(IpcContext& ctx, s64 ret, int err) {
    ctx.Push<s32>(s32(ret));
    ctx.Push<u32>(ret < 0 ? u32(err) : 0u);
    ctx.SetResult(Result::Success);
}

// sockaddr de la consola (FreeBSD): u8 len, u8 family, u16 puerto, u32 ip, 8 bytes a cero
bool ParseAddr(const std::vector<u8>& raw, Net::Addr4& out) {
    if (raw.size() < 8 || raw[1] != BSD_AF_INET) return false;
    std::memcpy(out.port, raw.data() + 2, 2);
    std::memcpy(out.ip, raw.data() + 4, 4);
    return true;
}
std::vector<u8> MakeAddr(const Net::Addr4& a) {
    std::vector<u8> raw(16, 0);
    raw[0] = 16;
    raw[1] = BSD_AF_INET;
    std::memcpy(raw.data() + 2, a.port, 2);
    std::memcpy(raw.data() + 4, a.ip, 4);
    return raw;
}

// struct timeval de la consola (s64 segundos, s64 microsegundos) -> ms (0 = sin limite)
int TimevalMs(const std::vector<u8>& raw) {
    if (raw.size() < 16) return -1;
    s64 sec = 0, usec = 0;
    std::memcpy(&sec, raw.data(), 8);
    std::memcpy(&usec, raw.data() + 8, 8);
    const s64 ms = sec * 1000 + usec / 1000;
    return ms <= 0 ? -1 : int(std::min<s64>(ms, 0x7FFFFFFF));
}

// Operacion que puede tener que esperar: si el socket del programa es bloqueante, repite
// esperando al PC hasta que se pueda (o se acabe su timeout); si no, devuelve EAGAIN.
template <typename Op>
long Blocking(IpcContext& ctx, GuestSocket& s, bool dontwait, short events, int timeout_ms, int& err, Op op) {
    for (;;) {
        const long n = op(err);
        if (n >= 0 || err != E::AGAIN || s.nonblocking || dontwait) return n;
        const int ev = WaitSocket(ctx.GetKernel(), s.handle, events, timeout_ms);
        if (ev == 0) { err = E::AGAIN; return -1; }   // SO_RCVTIMEO/SNDTIMEO: EAGAIN
        if (ev < 0)  { err = E::INTR;  return -1; }
    }
}

} // namespace

BsdService::BsdService(std::string name) : ServiceObject(std::move(name)) {
    // RegisterClient(config, pid, tamano de la memoria de transferencia, handle) -> u64 pid
    RegisterCommand(0, "RegisterClient", [](IpcContext& ctx) {
        ctx.GetKernel().Network();
        ctx.Push<u64>(1);
        ctx.SetResult(Result::Success);
    });
    RegisterStub(1, "StartMonitoring");

    // Socket / SocketExempt(domain, type, protocol) -> fd
    auto socket = [](IpcContext& ctx) {
        const s32 domain = ctx.Pop<s32>(), type = ctx.Pop<s32>(), protocol = ctx.Pop<s32>();
        if (domain != BSD_AF_INET) { Reply(ctx, -1, E::AFNOSUPPORT); return; }
        const int base = type & ~(BSD_SOCK_NONBLOCK | BSD_SOCK_CLOEXEC);
        if (base < 1 || base > 3) { Reply(ctx, -1, E::PROTONOSUPPORT); return; }
        int err = 0;
        const Net::Handle h = Net::Socket(base, protocol, err);
        if (h == Net::INVALID_HANDLE) { Reply(ctx, -1, err); return; }
        GuestSocket s;
        s.handle = h;
        s.type = base;
        s.nonblocking = (type & BSD_SOCK_NONBLOCK) != 0;
        Reply(ctx, ctx.GetKernel().Network().Add(s), 0);
    };
    RegisterCommand(2, "Socket", socket);
    RegisterCommand(3, "SocketExempt", socket);
    RegisterCommand(4, "Open", [](IpcContext& ctx) { Reply(ctx, -1, E::OPNOTSUPP); });

    // Select(nfds, timeval, es_nulo) + 3 fd_set de entrada y 3 de salida. libnx usa Poll;
    // esto es por si algun programa lo llama directamente.
    RegisterCommand(5, "Select", [](IpcContext& ctx) {
        const s32 nfds = ctx.Pop<s32>();
        ctx.Pop<s32>();
        const s64 sec = ctx.Pop<s64>(), usec = ctx.Pop<s64>();
        const bool infinite = ctx.Pop<u8>() != 0;
        std::vector<u8> sets[3] = {ctx.ReadBuffer(0), ctx.ReadBuffer(1), ctx.ReadBuffer(2)};
        auto& net = ctx.GetKernel().Network();
        std::vector<Net::PollEntry> entries;
        std::vector<s32> fds;
        for (s32 fd = 0; fd < nfds; ++fd) {
            short ev = 0;
            for (int k = 0; k < 2; ++k)
                if (size_t(fd / 8) < sets[k].size() && (sets[k][fd / 8] >> (fd % 8) & 1)) ev |= k == 0 ? POLL_IN : POLL_OUT;
            if (!ev) continue;
            GuestSocket* s = net.Get(fd);
            entries.push_back({s ? s->handle : Net::INVALID_HANDLE, ev, 0});
            fds.push_back(fd);
        }
        const int timeout = infinite ? -1 : int(sec * 1000 + usec / 1000);
        int err = 0, n = 0;
        using Clock = std::chrono::steady_clock;
        const auto end = Clock::now() + std::chrono::milliseconds(std::max(timeout, 0));
        for (;;) {
            const bool released = ctx.GetKernel().ReleaseLockForWait();
            n = Net::Poll(entries, timeout == 0 ? 0 : 20, err);
            ctx.GetKernel().ReacquireLockAfterWait(released);
            if (n != 0 || timeout == 0 || ctx.GetKernel().StopRequested()) break;
            if (timeout > 0 && Clock::now() >= end) break;
        }
        if (n < 0) { Reply(ctx, -1, err); return; }
        for (auto& set : sets) std::fill(set.begin(), set.end(), u8(0));
        n = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            const s32 fd = fds[i];
            const short r = entries[i].revents;
            if ((r & (POLL_IN | 0x10)) && size_t(fd / 8) < sets[0].size()) { sets[0][fd / 8] |= u8(1 << (fd % 8)); ++n; }
            if ((r & POLL_OUT) && size_t(fd / 8) < sets[1].size()) { sets[1][fd / 8] |= u8(1 << (fd % 8)); ++n; }
            if ((r & 8) && size_t(fd / 8) < sets[2].size()) { sets[2][fd / 8] |= u8(1 << (fd % 8)); ++n; }
        }
        for (int k = 0; k < 3; ++k) ctx.WriteBuffer(sets[k].data(), sets[k].size(), size_t(k));
        Reply(ctx, n, 0);
    });

    // Poll(nfds, timeout) + pollfd[] (s32 fd, s16 events, s16 revents) de entrada y salida
    RegisterCommand(6, "Poll", [](IpcContext& ctx) {
        const u32 nfds = ctx.Pop<u32>();
        const s32 timeout = ctx.Pop<s32>();
        std::vector<u8> raw = ctx.ReadBuffer(0);
        const size_t count = std::min<size_t>(nfds, raw.size() / 8);
        auto& net = ctx.GetKernel().Network();
        std::vector<Net::PollEntry> entries(count);
        for (size_t i = 0; i < count; ++i) {
            s32 fd; s16 events;
            std::memcpy(&fd, raw.data() + i * 8, 4);
            std::memcpy(&events, raw.data() + i * 8 + 4, 2);
            GuestSocket* s = fd >= 0 ? net.Get(fd) : nullptr;
            entries[i] = {fd < 0 ? Net::INVALID_HANDLE : (s ? s->handle : Net::INVALID_HANDLE), events, 0};
        }
        int err = 0, n = 0;
        using Clock = std::chrono::steady_clock;
        const auto end = Clock::now() + std::chrono::milliseconds(std::max(timeout, 0));
        for (;;) {
            const bool released = ctx.GetKernel().ReleaseLockForWait();
            n = Net::Poll(entries, timeout == 0 ? 0 : 20, err);
            ctx.GetKernel().ReacquireLockAfterWait(released);
            if (n != 0 || timeout == 0 || ctx.GetKernel().StopRequested()) break;
            if (timeout > 0 && Clock::now() >= end) break;
        }
        if (n < 0) { Reply(ctx, -1, err); return; }
        for (size_t i = 0; i < count; ++i) {
            s32 fd;
            std::memcpy(&fd, raw.data() + i * 8, 4);
            const s16 revents = fd < 0 ? 0 : entries[i].revents;   // fd negativo: se ignora (POSIX)
            std::memcpy(raw.data() + i * 8 + 6, &revents, 2);
        }
        ctx.WriteBuffer(raw.data(), raw.size(), 0);
        Reply(ctx, n, 0);
    });
    RegisterCommand(7, "Sysctl", [](IpcContext& ctx) { Reply(ctx, -1, E::OPNOTSUPP); });

    // Recv(fd, flags) -> datos / RecvFrom(fd, flags) -> datos, direccion, u32 tamano
    auto recv = [](bool from) {
        return [from](IpcContext& ctx) {
            const s32 fd = ctx.Pop<s32>(), flags = ctx.Pop<s32>();
            GuestSocket* s = ctx.GetKernel().Network().Get(fd);
            if (!s) { Reply(ctx, -1, E::BADF); if (from) ctx.Push<u32>(0); return; }
            std::vector<u8> data(size_t(ctx.GetWriteBufferSize(0)));
            Net::Addr4 addr;
            int err = 0;
            const bool peek = flags & BSD_MSG_PEEK;
            const long n = Blocking(ctx, *s, flags & BSD_MSG_DONTWAIT, POLL_IN, s->recv_timeout_ms, err, [&](int& e) {
                return from ? Net::RecvFrom(s->handle, data.data(), data.size(), peek, addr, e)
                            : Net::Recv(s->handle, data.data(), data.size(), peek, e);
            });
            if (n > 0) ctx.WriteBuffer(data.data(), size_t(n), 0);
            Reply(ctx, n, err);
            if (from) {
                if (n >= 0) {
                    const auto raw = MakeAddr(addr);
                    ctx.WriteBuffer(raw.data(), raw.size(), 1);
                }
                ctx.Push<u32>(n >= 0 ? 16u : 0u);
            }
        };
    };
    RegisterCommand(8, "Recv", recv(false));
    RegisterCommand(9, "RecvFrom", recv(true));

    // Send(fd, flags) + datos / SendTo(fd, flags) + datos, direccion
    auto send = [](bool to) {
        return [to](IpcContext& ctx) {
            const s32 fd = ctx.Pop<s32>(), flags = ctx.Pop<s32>();
            GuestSocket* s = ctx.GetKernel().Network().Get(fd);
            if (!s) { Reply(ctx, -1, E::BADF); return; }
            const std::vector<u8> data = ctx.ReadBuffer(0);
            Net::Addr4 addr;
            if (to && !ParseAddr(ctx.ReadBuffer(1), addr)) { Reply(ctx, -1, E::INVAL); return; }
            int err = 0;
            const long n = Blocking(ctx, *s, flags & BSD_MSG_DONTWAIT, POLL_OUT, s->send_timeout_ms, err, [&](int& e) {
                return to ? Net::SendTo(s->handle, data.data(), data.size(), addr, e)
                          : Net::Send(s->handle, data.data(), data.size(), false, e);
            });
            Reply(ctx, n, err);
        };
    };
    RegisterCommand(10, "Send", send(false));
    RegisterCommand(11, "SendTo", send(true));

    // Accept(fd) -> fd nuevo, direccion, u32 tamano
    RegisterCommand(12, "Accept", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>();
        auto& net = ctx.GetKernel().Network();
        GuestSocket* s = net.Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); ctx.Push<u32>(0); return; }
        Net::Addr4 addr;
        Net::Handle h = Net::INVALID_HANDLE;
        int err = 0;
        const GuestSocket listener = *s;
        const long r = Blocking(ctx, *s, false, POLL_IN, -1, err, [&](int& e) {
            h = Net::Accept(listener.handle, addr, e);
            return h == Net::INVALID_HANDLE ? -1L : 0L;
        });
        if (r < 0) { Reply(ctx, -1, err); ctx.Push<u32>(0); return; }
        GuestSocket ns;
        ns.handle = h;
        ns.type = listener.type;
        const s32 nfd = net.Add(ns);
        const auto raw = MakeAddr(addr);
        ctx.WriteBuffer(raw.data(), raw.size(), 0);
        Reply(ctx, nfd, 0);
        ctx.Push<u32>(16);
    });

    RegisterCommand(13, "Bind", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        Net::Addr4 addr;
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        if (!ParseAddr(ctx.ReadBuffer(0), addr)) { Reply(ctx, -1, E::INVAL); return; }
        int err = 0;
        Reply(ctx, Net::Bind(s->handle, addr, err), err);
    });

    RegisterCommand(14, "Connect", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        Net::Addr4 addr;
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        if (!ParseAddr(ctx.ReadBuffer(0), addr)) { Reply(ctx, -1, E::INVAL); return; }
        Logger::Log(Logger::Level::Info, "[bsd] Connect(" + std::to_string(addr.ip[0]) + "." + std::to_string(addr.ip[1]) + "." +
                    std::to_string(addr.ip[2]) + "." + std::to_string(addr.ip[3]) + ":" +
                    std::to_string((addr.port[0] << 8) | addr.port[1]) + ")");
        int err = 0;
        if (Net::Connect(s->handle, addr, err) == 0) { Reply(ctx, 0, 0); return; }
        if (err != E::INPROGRESS || s->nonblocking) { Reply(ctx, -1, err); return; }
        // Socket bloqueante: esperar a que conecte (como mucho 30 s, o su SO_SNDTIMEO)
        const Net::Handle h = s->handle;
        const int ev = WaitSocket(ctx.GetKernel(), h, POLL_OUT, s->send_timeout_ms > 0 ? s->send_timeout_ms : 30000);
        if (ev == 0) { Reply(ctx, -1, E::TIMEDOUT); return; }
        if (ev < 0)  { Reply(ctx, -1, E::INTR); return; }
        const int pending = Net::PendingError(h);
        Reply(ctx, pending ? -1 : 0, pending);
    });

    // GetPeerName / GetSockName(fd) -> direccion, u32 tamano
    auto sock_name = [](bool peer) {
        return [peer](IpcContext& ctx) {
            const s32 fd = ctx.Pop<s32>();
            GuestSocket* s = ctx.GetKernel().Network().Get(fd);
            if (!s) { Reply(ctx, -1, E::BADF); ctx.Push<u32>(0); return; }
            Net::Addr4 addr;
            int err = 0;
            const int r = peer ? Net::GetPeerName(s->handle, addr, err) : Net::GetSockName(s->handle, addr, err);
            if (r == 0) {
                const auto raw = MakeAddr(addr);
                ctx.WriteBuffer(raw.data(), raw.size(), 0);
            }
            Reply(ctx, r, err);
            ctx.Push<u32>(r == 0 ? 16u : 0u);
        };
    };
    RegisterCommand(15, "GetPeerName", sock_name(true));
    RegisterCommand(16, "GetSockName", sock_name(false));

    // GetSockOpt(fd, level, name) -> valor, u32 tamano
    RegisterCommand(17, "GetSockOpt", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>(), level = ctx.Pop<s32>(), opt = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); ctx.Push<u32>(0); return; }
        std::vector<u8> value(std::max<size_t>(size_t(ctx.GetWriteBufferSize(0)), 4), 0);
        size_t size = value.size();
        int err = 0, r = 0;
        if (level == BSD_SOL_SOCKET && (opt == BSD_SO_RCVTIMEO || opt == BSD_SO_SNDTIMEO)) {
            const int ms = opt == BSD_SO_RCVTIMEO ? s->recv_timeout_ms : s->send_timeout_ms;
            const s64 tv[2] = {ms > 0 ? ms / 1000 : 0, ms > 0 ? (ms % 1000) * 1000 : 0};
            value.assign(16, 0);
            std::memcpy(value.data(), tv, 16);
            size = 16;
        } else if (level == BSD_SOL_SOCKET && opt == BSD_SO_TYPE) {
            std::memcpy(value.data(), &s->type, 4);
            size = 4;
        } else {
            r = Net::GetSockOpt(s->handle, level, opt, value.data(), size, err);
        }
        if (r == 0) ctx.WriteBuffer(value.data(), size, 0);
        Reply(ctx, r, err);
        ctx.Push<u32>(r == 0 ? u32(size) : 0u);
    });

    RegisterCommand(18, "Listen", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>(), backlog = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        int err = 0;
        Reply(ctx, Net::Listen(s->handle, backlog, err), err);
    });
    RegisterCommand(19, "Ioctl", [](IpcContext& ctx) { Reply(ctx, -1, E::OPNOTSUPP); });

    // Fcntl(fd, cmd, flags): solo O_NONBLOCK
    RegisterCommand(20, "Fcntl", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>(), cmd = ctx.Pop<s32>(), flags = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        if (cmd == BSD_F_GETFL) { Reply(ctx, s->nonblocking ? NX_O_NONBLOCK : 0, 0); return; }
        if (cmd == BSD_F_SETFL) { s->nonblocking = (flags & NX_O_NONBLOCK) != 0; Reply(ctx, 0, 0); return; }
        Reply(ctx, -1, E::INVAL);
    });

    // SetSockOpt(fd, level, name) + valor
    RegisterCommand(21, "SetSockOpt", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>(), level = ctx.Pop<s32>(), opt = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        const std::vector<u8> value = ctx.ReadBuffer(0);
        if (level == BSD_SOL_SOCKET && opt == BSD_SO_RCVTIMEO) { s->recv_timeout_ms = TimevalMs(value); Reply(ctx, 0, 0); return; }
        if (level == BSD_SOL_SOCKET && opt == BSD_SO_SNDTIMEO) { s->send_timeout_ms = TimevalMs(value); Reply(ctx, 0, 0); return; }
        if (level == BSD_SOL_SOCKET && opt == BSD_SO_NOSIGPIPE) { Reply(ctx, 0, 0); return; }
        int err = 0;
        Reply(ctx, Net::SetSockOpt(s->handle, level, opt, value.data(), value.size(), err), err);
    });

    RegisterCommand(22, "Shutdown", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>(), how = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        int err = 0;
        Reply(ctx, Net::Shutdown(s->handle, how, err), err);
    });
    RegisterCommand(23, "ShutdownAllSockets", [](IpcContext& ctx) { Reply(ctx, 0, 0); });

    // Write(fd) + datos / Read(fd) -> datos: como Send/Recv sin flags
    RegisterCommand(24, "Write", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        const std::vector<u8> data = ctx.ReadBuffer(0);
        int err = 0;
        const long n = Blocking(ctx, *s, false, POLL_OUT, s->send_timeout_ms, err, [&](int& e) {
            return Net::Send(s->handle, data.data(), data.size(), false, e);
        });
        Reply(ctx, n, err);
    });
    RegisterCommand(25, "Read", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(fd);
        if (!s) { Reply(ctx, -1, E::BADF); return; }
        std::vector<u8> data(size_t(ctx.GetWriteBufferSize(0)));
        int err = 0;
        const long n = Blocking(ctx, *s, false, POLL_IN, s->recv_timeout_ms, err, [&](int& e) {
            return Net::Recv(s->handle, data.data(), data.size(), false, e);
        });
        if (n > 0) ctx.WriteBuffer(data.data(), size_t(n), 0);
        Reply(ctx, n, err);
    });
    RegisterCommand(26, "Close", [](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>();
        auto& net = ctx.GetKernel().Network();
        if (!net.Get(fd)) { Reply(ctx, -1, E::BADF); return; }
        net.Remove(fd);
        Reply(ctx, 0, 0);
    });
    RegisterCommand(27, "DuplicateSocket", [](IpcContext& ctx) { Reply(ctx, -1, E::OPNOTSUPP); });
}

// ============================================================================
//  sfdnsres: DNS
// ============================================================================
namespace {
constexpr u32 ADDRINFO_MAGIC = 0xBEEFCAFE;
void PutBE32(std::vector<u8>& out, u32 v) {
    out.push_back(u8(v >> 24)); out.push_back(u8(v >> 16)); out.push_back(u8(v >> 8)); out.push_back(u8(v));
}
u32 GetBE32(const std::vector<u8>& in, size_t off) {
    if (off + 4 > in.size()) return 0;
    return (u32(in[off]) << 24) | (u32(in[off + 1]) << 16) | (u32(in[off + 2]) << 8) | in[off + 3];
}
std::string CString(const std::vector<u8>& raw) {
    return std::string(raw.begin(), std::find(raw.begin(), raw.end(), u8(0)));
}
// Puerto de un "servicio" de getaddrinfo: un numero, o los nombres de siempre
int ServicePort(const std::string& service) {
    if (service.empty()) return 0;
    if (std::all_of(service.begin(), service.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::atoi(service.c_str());
    if (service == "http") return 80;
    if (service == "https") return 443;
    if (service == "ftp") return 21;
    return -1;
}
} // namespace

SfdnsresService::SfdnsresService() : ServiceObject("sfdnsres") {
    // GetAddrInfoRequest(use_nsd, cancel, pid) + nombre, servicio, pistas -> direcciones
    //   -> u32 errno, s32 resultado (EAI_*), u32 tamano
    RegisterCommand(6, "GetAddrInfoRequest", [](IpcContext& ctx) {
        const std::string host = CString(ctx.ReadBuffer(0));
        const std::string service = CString(ctx.ReadBuffer(1));
        const std::vector<u8> hints = ctx.ReadBuffer(2);
        s32 socktype = 1, protocol = 6;   // por defecto TCP
        if (GetBE32(hints, 0) == ADDRINFO_MAGIC) {
            if (const s32 t = s32(GetBE32(hints, 12))) socktype = t;
            if (const s32 p = s32(GetBE32(hints, 16))) protocol = p;
            else if (socktype == 2) protocol = 17;
        }
        auto finish = [&ctx](u32 err, s32 ret, u32 size) {
            ctx.Push<u32>(err);
            ctx.Push<s32>(ret);
            ctx.Push<u32>(size);
            ctx.SetResult(Result::Success);
        };
        const int port = ServicePort(service);
        if (port < 0) { finish(0, 9 /* EAI_SERVICE */, 0); return; }
        std::vector<Net::Addr4> addrs;
        int rc = 0;
        if (host.empty()) {
            addrs.emplace_back();   // 0.0.0.0
        } else {
            const bool released = ctx.GetKernel().ReleaseLockForWait();   // el DNS puede tardar
            rc = Net::Resolve(host, addrs);
            ctx.GetKernel().ReacquireLockAfterWait(released);
        }
        Logger::Log(Logger::Level::Info, "[sfdnsres] " + host + (rc ? ": no encontrado" :
                    ": " + std::to_string(addrs.size()) + " direcciones"));
        if (rc != 0) { finish(0, rc, 0); return; }
        // Formato de libnx (todo en big endian, y la direccion "doblemente" girada: ver resolver.c)
        std::vector<u8> out;
        for (const auto& a : addrs) {
            PutBE32(out, ADDRINFO_MAGIC);
            PutBE32(out, 0);                // flags
            PutBE32(out, BSD_AF_INET);      // family
            PutBE32(out, u32(socktype));
            PutBE32(out, u32(protocol));
            PutBE32(out, 16);               // addrlen
            const u8 sockaddr[16] = {16, BSD_AF_INET, u8(port), u8(port >> 8),
                                     a.ip[3], a.ip[2], a.ip[1], a.ip[0]};
            out.insert(out.end(), sockaddr, sockaddr + 16);
            out.push_back(0);               // sin nombre canonico
        }
        PutBE32(out, 0);                    // fin de la lista
        if (out.size() > ctx.GetWriteBufferSize(0)) { finish(0, 10 /* EAI_OVERFLOW */, 0); return; }
        ctx.WriteBuffer(out.data(), out.size(), 0);
        finish(0, 0, u32(out.size()));
    });
    RegisterCommand(8, "GetCancelHandleRequest", [](IpcContext& ctx) { ctx.Push<u32>(1); ctx.SetResult(Result::Success); });
    RegisterStub(9, "CancelRequest");
}

// ============================================================================
//  nifm: siempre conectada (la red del PC)
// ============================================================================
NifmService::NifmService(std::string name) : ServiceObject(std::move(name)) {
    auto create = [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<NifmGeneralService>());
        ctx.SetResult(Result::Success);
    };
    RegisterCommand(4, "CreateGeneralServiceOld", create);
    RegisterCommand(5, "CreateGeneralService", create);
}

NifmGeneralService::NifmGeneralService() : ServiceObject("IGeneralService") {
    // GetClientId -> buffer con el id
    RegisterCommand(1, "GetClientId", [](IpcContext& ctx) {
        const u32 id = 1;
        ctx.WriteBuffer(&id, sizeof(id), 0);
        ctx.SetResult(Result::Success);
    });
    // GetCurrentIpConfigInfo: IP, mascara, puerta de enlace, DNS (como una red de casa)
    RegisterCommand(15, "GetCurrentIpConfigInfo", [](IpcContext& ctx) {
        const Net::Addr4 a = Net::LocalAddress();
        const u8 mask[4] = {255, 255, 255, 0};
        const u8 gateway[4] = {a.ip[0], a.ip[1], a.ip[2], 1};
        ctx.Push<u8>(1);                    // IP automatica
        ctx.PushBytes(a.ip, 4);
        ctx.PushBytes(mask, 4);
        ctx.PushBytes(gateway, 4);
        ctx.Push<u8>(1);                    // DNS automatico
        ctx.PushBytes(gateway, 4);
        const u8 dns2[4] = {8, 8, 8, 8};
        ctx.PushBytes(dns2, 4);
        ctx.SetResult(Result::Success);
    });
    // GetCurrentNetworkProfile: perfil vacio (sin proxy)
    RegisterCommand(5, "GetCurrentNetworkProfile", [](IpcContext& ctx) {
        const std::vector<u8> zeros(size_t(ctx.GetWriteBufferSize(0)), 0);
        ctx.WriteBuffer(zeros.data(), zeros.size(), 0);
        ctx.SetResult(Result::Success);
    });
    // GetCurrentIpAddress: la IP del PC (en orden de red)
    RegisterCommand(12, "GetCurrentIpAddress", [](IpcContext& ctx) {
        const Net::Addr4 a = Net::LocalAddress();
        ctx.PushBytes(a.ip, 4);
        ctx.SetResult(Result::Success);
    });
    // GetInternetConnectionStatus: wifi, senal 3, conectada (4)
    RegisterCommand(18, "GetInternetConnectionStatus", [](IpcContext& ctx) {
        ctx.Push<u8>(1); ctx.Push<u8>(3); ctx.Push<u8>(4);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(20, "IsEthernetCommunicationEnabled", [](IpcContext& ctx) { ctx.Push<u8>(0); ctx.SetResult(Result::Success); });
    RegisterCommand(21, "IsAnyInternetRequestAccepted", [](IpcContext& ctx) { ctx.Push<u8>(1); ctx.SetResult(Result::Success); });
    RegisterCommand(22, "IsAnyForegroundRequestAccepted", [](IpcContext& ctx) { ctx.Push<u8>(1); ctx.SetResult(Result::Success); });
    RegisterCommand(17, "IsWirelessCommunicationEnabled", [](IpcContext& ctx) { ctx.Push<u8>(1); ctx.SetResult(Result::Success); });
}

} // namespace NeXo2::HLE
