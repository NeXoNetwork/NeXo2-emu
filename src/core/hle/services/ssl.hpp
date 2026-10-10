#pragma once
// "ssl": TLS de la consola (https). libcurl de devkitPro lo usa a traves de libnx.
// NeXo hace el TLS de verdad con mbedTLS (externals/mbedtls) sobre el socket del PC que
// el programa abrio con bsd, y comprueba los servidores con los certificados raiz del PC.
// Sin mbedTLS (NEXO2_ENABLE_TLS=OFF) las conexiones fallan con un error claro.
// Ver docs/07-nexo-internals/network.md.
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "hle/service.hpp"

namespace NeXo2::HLE {

// Lo que guarda un contexto (certificados que el programa importa) y comparten sus conexiones
struct SslContextData {
    struct Pki { std::vector<u8> data; bool pem = false; };
    std::vector<Pki> server_pki;
};

class SslService final : public ServiceObject {
public:
    SslService();
};

class SslContext final : public ServiceObject {
public:
    SslContext();
private:
    std::shared_ptr<SslContextData> m_data = std::make_shared<SslContextData>();
    u64 m_nextId = 1;
    std::map<u32, s32> m_options;
};

struct TlsSession;   // ssl.cpp (mbedTLS)

class SslConnection final : public ServiceObject {
public:
    explicit SslConnection(std::shared_ptr<SslContextData> context);
    ~SslConnection() override;

private:
    u32 Handshake(IpcContext& ctx);
    u32 ReadSome(IpcContext& ctx, std::vector<u8>& out, size_t max, bool consume);

    std::shared_ptr<SslContextData> m_context;
    std::unique_ptr<TlsSession> m_tls;
    s32  m_socket = -1;
    std::string m_host;
    u32  m_verify = 3;           // PeerCa | HostName (lo de la consola por defecto)
    u32  m_ioMode = 1;           // 1 = bloqueante, 2 = no bloqueante
    u32  m_sessionCacheMode = 0, m_renegotiationMode = 0;
    std::map<u32, bool> m_options;
    std::vector<u8> m_alpn;      // protocolos ALPN que pide el programa (formato de la red)
    std::vector<u8> m_peek;      // datos ya descifrados que el programa miro con Peek
    bool m_handshakeDone = false;
    u32  m_verifyError = 0;
};

} // namespace NeXo2::HLE
