# Network: sockets, DNS and TLS

Programs reach the internet through the PC's network. Three services do it, with the formats
libnx uses (`services/bsd.c`, `sfdnsres.c`, `ssl.c`, `runtime/resolver.c`):

| Service | File | What it does |
| :--- | :--- | :--- |
| `bsd:u`, `bsd:s` | `src/core/hle/services/bsd.cpp` | TCP/UDP IPv4 sockets -> sockets of the PC |
| `sfdnsres` | same | `getaddrinfo` -> the PC's DNS |
| `nifm:u` | same | Network status: connected (Wi-Fi, full signal), the PC's IP |
| `ssl` | `src/core/hle/services/ssl.cpp` | TLS (https) with mbedTLS over the program's socket |

The PC side is isolated in `src/core/hle/net/`: `host_net.*` (Winsock or POSIX; the only file
that includes those headers) and `host_certs.*` (root certificates of the PC).

## Sockets (bsd)

- Every reply is `s32 result, u32 errno`. The errno numbers are **Linux** ones: Nintendo built
  its FreeBSD stack with Linux errno values and libnx converts them. Socket options, `MSG_*`
  flags and `sockaddr` (`u8 len, u8 family, u16 port, u32 ip`) are FreeBSD ones.
  `host_net.cpp` translates both ways.
- The guest's sockets live in `NetworkState` (one per process, `Kernel::Network()`); Kernel
  reset closes them.
- The PC sockets are always non-blocking. When the program's socket is blocking, the service
  retries and waits with `WaitSocket`: `poll` in 20 ms slices **without the kernel lock**
  (`Kernel::ReleaseLockForWait`), so the other cores keep running and stopping the emulator
  interrupts the wait. `SO_RCVTIMEO`/`SO_SNDTIMEO` are honoured (timeout -> `EAGAIN`).
  `fcntl(F_SETFL, O_NONBLOCK)` uses Linux's 0x800, like libnx.
- Implemented: Socket, Select, Poll, Recv/RecvFrom, Send/SendTo, Accept, Bind, Connect,
  GetPeerName/GetSockName, Get/SetSockOpt, Listen, Fcntl, Shutdown, Write, Read, Close.

## DNS (sfdnsres)

`GetAddrInfoRequest` (6) resolves with the PC and answers in libnx's serialized format: a
big-endian header (`0xBEEFCAFE`, flags, family, socktype, protocol, addrlen) per address,
followed by the `sockaddr_in` "byte-swapped twice" (port and IP stored little-endian, as
`resolver.c` expects), an empty canonical name, and a final zero.

## TLS (ssl)

libcurl from devkitPro uses the ssl service (`lib/vtls/libnx.c` in its patch):
CreateContext -> ImportServerPki (optional) -> CreateConnection -> SetOption(DoNotCloseSocket)
-> SetSocketDescriptor -> SetHostName -> SetVerifyOption -> SetIoMode -> DoHandshake(GetServerCert)
-> Write / Read.

- The TLS itself is **mbedTLS 3.6** (`externals/mbedtls`, a submodule with its own `framework`
  submodule). TLS 1.2 and 1.3, SNI, ALPN.
- Certificates: the PC's roots (Windows: the `ROOT` and `CA` system stores; Linux: the distro
  bundle) plus whatever the program imports. `NEXO2_CA_FILE=file.pem` adds more.
  `SslVerifyOption`: PeerCa -> the server must be trusted; HostName and DateCheck can be
  switched off by the program.
- Results: would block = `(123, 204)`, verification failed = `(123, 207)` (what curl checks).
  Blocking mode waits like bsd (up to 5 minutes, the console's timeout); non-blocking returns
  "would block" and the program calls again.
- DoHandshakeGetServerCert returns the server certificate, or the whole chain in the
  "CertChMN" format when GetServerCertChain is on.
- Without mbedTLS (`NEXO2_ENABLE_TLS=OFF` or missing submodule) programs still have plain
  sockets; every TLS operation fails with a clear log line.

## Tests

`tests/network_tests.cpp`: a TCP server and client on 127.0.0.1 (connect in progress, accept,
EAGAIN with no data, peek, options with console numbers, end of stream) and DNS of localhost.

Checked by hand: the Homebrew App Store downloads its package list over https (TLS 1.3, the
certificate verified) and shows the apps.
