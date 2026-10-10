#include "ssl.hpp"
#include "bsd.hpp"
#include "hle/kernel.hpp"
#include "hle/net/host_certs.hpp"
#include "hle/net/host_net.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef NEXO2_HAS_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif

namespace NeXo2::HLE {

using Common::Logger;

namespace {
// Resultados del servicio ssl (modulo 123), los que mira libcurl
constexpr u32 SslResult(u32 desc) { return (desc << 9) | 123u; }
constexpr u32 RESULT_WOULD_BLOCK      = SslResult(204);   // PR_WOULD_BLOCK_ERROR
constexpr u32 RESULT_VERIFY_FAILED    = SslResult(207);
constexpr u32 RESULT_CONNECTION_ERROR = SslResult(205);
constexpr u32 RESULT_NO_SOCKET        = SslResult(103);
constexpr u32 RESULT_CERT_ERROR       = SslResult(301);
constexpr u32 RESULT_NOT_AVAILABLE    = SslResult(9);     // NeXo compilado sin mbedTLS

constexpr u32 OPTION_DO_NOT_CLOSE_SOCKET = 0;
constexpr u32 OPTION_GET_SERVER_CERT_CHAIN = 1;
constexpr u32 VERIFY_PEER_CA = 1, VERIFY_HOST_NAME = 2, VERIFY_DATE_CHECK = 4;
constexpr u32 IO_NON_BLOCKING = 2;
constexpr int BLOCKING_TIMEOUT_MS = 5 * 60 * 1000;   // modo bloqueante de la consola: 5 minutos
constexpr short POLL_IN = 1, POLL_OUT = 4;
}

// ============================================================================
//  Sesion TLS (mbedTLS)
// ============================================================================
#ifdef NEXO2_HAS_TLS
struct TlsSession {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    Net::Handle sock = Net::INVALID_HANDLE;
    std::vector<std::string> alpn;
    std::vector<const char*> alpn_ptrs;
    u32 verify = 0;

    TlsSession() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&ca);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~TlsSession() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&ca);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }

    // El socket del PC es no bloqueante: "no hay datos" -> WANT_READ/WANT_WRITE
    static int Send(void* self, const unsigned char* buf, size_t len) {
        auto* s = static_cast<TlsSession*>(self);
        int err = 0;
        const long n = Net::Send(s->sock, buf, len, false, err);
        if (n >= 0) return int(n);
        return err == Net::Errno::AGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int Recv(void* self, unsigned char* buf, size_t len) {
        auto* s = static_cast<TlsSession*>(self);
        int err = 0;
        const long n = Net::Recv(s->sock, buf, len, false, err);
        if (n >= 0) return int(n);
        return err == Net::Errno::AGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    // Lo que el programa no pidio comprobar no cuenta como error
    static int Verify(void* self, mbedtls_x509_crt*, int, uint32_t* flags) {
        const u32 v = static_cast<TlsSession*>(self)->verify;
        if (!(v & VERIFY_HOST_NAME)) *flags &= ~uint32_t(MBEDTLS_X509_BADCERT_CN_MISMATCH);
        if (!(v & VERIFY_DATE_CHECK)) *flags &= ~uint32_t(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
        return 0;
    }

    bool Setup(const std::string& host, u32 verify_option, const SslContextData& ctx_data, const std::vector<u8>& alpn_wire) {
        static std::once_flag psa_once;
        std::call_once(psa_once, [] { psa_crypto_init(); });
        verify = verify_option;
        const char* pers = "nexo2-ssl";
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const unsigned char*>(pers), std::strlen(pers)) != 0) return false;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
        // Certificados raiz: los del PC + los que importo el programa
        for (const auto& c : Net::SystemRootCerts())
            c.pem ? mbedtls_x509_crt_parse(&ca, c.data.data(), c.data.size())
                  : mbedtls_x509_crt_parse_der(&ca, c.data.data(), c.data.size());
        for (const auto& p : ctx_data.server_pki) {
            if (p.pem) {
                std::vector<u8> z = p.data;
                if (z.empty() || z.back() != 0) z.push_back(0);
                mbedtls_x509_crt_parse(&ca, z.data(), z.size());
            } else {
                mbedtls_x509_crt_parse_der(&ca, p.data.data(), p.data.size());
            }
        }
        mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
        mbedtls_ssl_conf_authmode(&conf, (verify & VERIFY_PEER_CA) ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_verify(&conf, &Verify, this);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        // ALPN: lista con un byte de longitud delante de cada nombre ("\x08http/1.1")
        for (size_t i = 0; i < alpn_wire.size();) {
            const size_t len = alpn_wire[i];
            if (len == 0 || i + 1 + len > alpn_wire.size()) break;
            alpn.emplace_back(reinterpret_cast<const char*>(alpn_wire.data() + i + 1), len);
            i += 1 + len;
        }
        if (!alpn.empty()) {
            for (const auto& a : alpn) alpn_ptrs.push_back(a.c_str());
            alpn_ptrs.push_back(nullptr);
            mbedtls_ssl_conf_alpn_protocols(&conf, alpn_ptrs.data());
        }
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return false;
        if (!host.empty() && mbedtls_ssl_set_hostname(&ssl, host.c_str()) != 0) return false;
        mbedtls_ssl_set_bio(&ssl, this, &Send, &Recv, nullptr);
        return true;
    }
};

namespace {
bool WantsIo(int ret) {
    return ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
           ret == MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS || ret == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS;
}
std::string TlsError(int ret) {
    char buf[160];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return buf;
}
}
#else
struct TlsSession {};
#endif

// ============================================================================
//  ssl
// ============================================================================

SslService::SslService() : ServiceObject("ssl") {
    // CreateContext(u32 version, u64 pid) -> ISslContext
    RegisterCommand(0, "CreateContext", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<SslContext>());
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "GetContextCount", [](IpcContext& ctx) { ctx.Push<u32>(0); ctx.SetResult(Result::Success); });
    RegisterCommand(3, "GetCertificateBufSize", [](IpcContext& ctx) { ctx.Push<u32>(0); ctx.SetResult(Result::Success); });
    RegisterStub(5, "SetInterfaceVersion");
    RegisterCommand(6, "FlushSessionCache", [](IpcContext& ctx) { ctx.Push<u32>(0); ctx.SetResult(Result::Success); });
    RegisterStub(7, "SetDebugOption");
    RegisterCommand(8, "GetDebugOption", [](IpcContext& ctx) { ctx.SetResult(Result::Success); });
    RegisterStub(9, "ClearTls12FallbackFlag");
}

SslContext::SslContext() : ServiceObject("ISslContext") {
    RegisterCommand(0, "SetOption", [this](IpcContext& ctx) {
        const u32 option = ctx.Pop<u32>();
        m_options[option] = ctx.Pop<s32>();
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "GetOption", [this](IpcContext& ctx) {
        const u32 option = ctx.Pop<u32>();
        ctx.Push<s32>(m_options.count(option) ? m_options[option] : 0);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(2, "CreateConnection", [this](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<SslConnection>(m_data));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(3, "GetConnectionCount", [](IpcContext& ctx) { ctx.Push<u32>(0); ctx.SetResult(Result::Success); });
    // ImportServerPki(u32 formato: 0 = PEM, 1 = DER) + certificado -> u64 id
    RegisterCommand(4, "ImportServerPki", [this](IpcContext& ctx) {
        const u32 format = ctx.Pop<u32>();
        m_data->server_pki.push_back({ctx.ReadBuffer(0), format == 0});
        ctx.Push<u64>(m_nextId++);
        ctx.SetResult(Result::Success);
    });
    // Certificados de cliente, CRL...: se aceptan (devuelven un id) pero no se usan
    auto new_id = [this](IpcContext& ctx) { ctx.Push<u64>(m_nextId++); ctx.SetResult(Result::Success); };
    RegisterCommand(5, "ImportClientPki", new_id);
    RegisterStub(6, "RemoveServerPki");
    RegisterStub(7, "RemoveClientPki");
    RegisterCommand(8, "RegisterInternalPki", new_id);
    RegisterStub(9, "AddPolicyOid");
    RegisterCommand(10, "ImportCrl", new_id);
    RegisterStub(11, "RemoveCrl");
    RegisterCommand(12, "ImportClientCertKeyPki", new_id);
}

SslConnection::~SslConnection() = default;

SslConnection::SslConnection(std::shared_ptr<SslContextData> context)
    : ServiceObject("ISslConnection"), m_context(std::move(context)) {
    // SetSocketDescriptor(fd) -> fd para el programa (-1 con DoNotCloseSocket: sigue usando el suyo)
    RegisterCommand(0, "SetSocketDescriptor", [this](IpcContext& ctx) {
        const s32 fd = ctx.Pop<s32>();
        if (!ctx.GetKernel().Network().Get(fd)) { ctx.SetResult(RESULT_NO_SOCKET); return; }
        m_socket = fd;
        ctx.Push<s32>(m_options[OPTION_DO_NOT_CLOSE_SOCKET] ? -1 : fd);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "SetHostName", [this](IpcContext& ctx) {
        const auto raw = ctx.ReadBuffer(0);
        m_host.assign(raw.begin(), std::find(raw.begin(), raw.end(), u8(0)));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(2, "SetVerifyOption", [this](IpcContext& ctx) { m_verify = ctx.Pop<u32>(); ctx.SetResult(Result::Success); });
    RegisterCommand(3, "SetIoMode", [this](IpcContext& ctx) { m_ioMode = ctx.Pop<u32>(); ctx.SetResult(Result::Success); });
    RegisterCommand(4, "GetSocketDescriptor", [this](IpcContext& ctx) { ctx.Push<s32>(m_socket); ctx.SetResult(Result::Success); });
    RegisterCommand(5, "GetHostName", [this](IpcContext& ctx) {
        ctx.WriteBuffer(m_host.c_str(), m_host.size() + 1, 0);
        ctx.Push<u32>(u32(m_host.size()));
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(6, "GetVerifyOption", [this](IpcContext& ctx) { ctx.Push<u32>(m_verify); ctx.SetResult(Result::Success); });
    RegisterCommand(7, "GetIoMode", [this](IpcContext& ctx) { ctx.Push<u32>(m_ioMode); ctx.SetResult(Result::Success); });

    RegisterCommand(8, "DoHandshake", [this](IpcContext& ctx) { ctx.SetResult(Handshake(ctx)); });
    // DoHandshakeGetServerCert -> u32 tamano, u32 cuantos + certificado(s) del servidor
    RegisterCommand(9, "DoHandshakeGetServerCert", [this](IpcContext& ctx) {
        const u32 rc = Handshake(ctx);
        u32 size = 0, total = 0;
#ifdef NEXO2_HAS_TLS
        if (rc == Result::Success && (m_verify & VERIFY_PEER_CA)) {
            std::vector<const mbedtls_x509_crt*> chain;
            for (const mbedtls_x509_crt* c = mbedtls_ssl_get_peer_cert(&m_tls->ssl); c && c->raw.p; c = c->next) chain.push_back(c);
            std::vector<u8> out;
            if (m_options[OPTION_GET_SERVER_CERT_CHAIN]) {
                // "CertChMN" + cuantos + (tamano, posicion) por certificado + los DER
                const u64 magic = 0x4E4D684374726543ull;
                out.resize(16 + chain.size() * 8);
                std::memcpy(out.data(), &magic, 8);
                const u32 n = u32(chain.size());
                std::memcpy(out.data() + 8, &n, 4);
                for (size_t i = 0; i < chain.size(); ++i) {
                    const u32 sz = u32(chain[i]->raw.len), off = u32(out.size());
                    std::memcpy(out.data() + 16 + i * 8, &sz, 4);
                    std::memcpy(out.data() + 16 + i * 8 + 4, &off, 4);
                    out.insert(out.end(), chain[i]->raw.p, chain[i]->raw.p + chain[i]->raw.len);
                }
            } else if (!chain.empty()) {
                out.assign(chain[0]->raw.p, chain[0]->raw.p + chain[0]->raw.len);
            }
            if (!out.empty() && out.size() <= ctx.GetWriteBufferSize(0)) {
                ctx.WriteBuffer(out.data(), out.size(), 0);
                size = u32(out.size());
                total = u32(chain.size());
            }
        }
#endif
        ctx.Push<u32>(size);
        ctx.Push<u32>(total);
        ctx.SetResult(rc);
    });
    // Read -> datos, u32 cuantos
    RegisterCommand(10, "Read", [this](IpcContext& ctx) {
        std::vector<u8> data;
        const u32 rc = ReadSome(ctx, data, size_t(ctx.GetWriteBufferSize(0)), true);
        if (!data.empty()) ctx.WriteBuffer(data.data(), data.size(), 0);
        ctx.Push<u32>(u32(data.size()));
        ctx.SetResult(rc);
    });
    // Write + datos -> u32 cuantos
    RegisterCommand(11, "Write", [this](IpcContext& ctx) {
        const std::vector<u8> data = ctx.ReadBuffer(0);
#ifdef NEXO2_HAS_TLS
        if (!m_handshakeDone) { ctx.Push<u32>(0); ctx.SetResult(RESULT_CONNECTION_ERROR); return; }
        GuestSocket* s = ctx.GetKernel().Network().Get(m_socket);
        if (!s) { ctx.Push<u32>(0); ctx.SetResult(RESULT_NO_SOCKET); return; }
        size_t written = 0;
        while (written < data.size()) {
            m_tls->sock = s->handle;
            const int ret = mbedtls_ssl_write(&m_tls->ssl, data.data() + written, data.size() - written);
            if (ret > 0) { written += size_t(ret); continue; }
            if (WantsIo(ret)) {
                if (written > 0) break;   // ya se envio algo: se devuelve eso
                if (m_ioMode == IO_NON_BLOCKING) { ctx.Push<u32>(0); ctx.SetResult(RESULT_WOULD_BLOCK); return; }
                const int ev = WaitSocket(ctx.GetKernel(), s->handle, ret == MBEDTLS_ERR_SSL_WANT_READ ? POLL_IN : POLL_OUT,
                                          BLOCKING_TIMEOUT_MS);
                s = ctx.GetKernel().Network().Get(m_socket);
                if (ev <= 0 || !s) { ctx.Push<u32>(0); ctx.SetResult(RESULT_CONNECTION_ERROR); return; }
                continue;
            }
            Logger::Log(Logger::Level::Info, "[ssl] Write: " + TlsError(ret));
            ctx.Push<u32>(0);
            ctx.SetResult(RESULT_CONNECTION_ERROR);
            return;
        }
        ctx.Push<u32>(u32(written));
        ctx.SetResult(Result::Success);
#else
        ctx.Push<u32>(0);
        ctx.SetResult(RESULT_NOT_AVAILABLE);
#endif
    });
    // Pending -> s32 bytes ya descifrados esperando
    RegisterCommand(12, "Pending", [this](IpcContext& ctx) {
        s32 n = s32(m_peek.size());
#ifdef NEXO2_HAS_TLS
        if (m_handshakeDone) n += s32(mbedtls_ssl_get_bytes_avail(&m_tls->ssl));
#endif
        ctx.Push<s32>(n);
        ctx.SetResult(Result::Success);
    });
    // Peek -> datos (sin consumirlos), u32 cuantos
    RegisterCommand(13, "Peek", [this](IpcContext& ctx) {
        std::vector<u8> data;
        const u32 rc = ReadSome(ctx, data, size_t(ctx.GetWriteBufferSize(0)), false);
        if (!data.empty()) ctx.WriteBuffer(data.data(), data.size(), 0);
        ctx.Push<u32>(u32(data.size()));
        ctx.SetResult(rc);
    });
    // Poll(eventos: 1 leer, 2 escribir, 4 excepcion; timeout ms) -> eventos listos
    RegisterCommand(14, "Poll", [this](IpcContext& ctx) {
        const u32 events = ctx.Pop<u32>(), timeout = ctx.Pop<u32>();
        GuestSocket* s = ctx.GetKernel().Network().Get(m_socket);
        u32 ready = 0;
        bool buffered = !m_peek.empty();
#ifdef NEXO2_HAS_TLS
        if (m_handshakeDone) buffered |= mbedtls_ssl_get_bytes_avail(&m_tls->ssl) > 0;
#endif
        if ((events & 1) && buffered) ready |= 1;
        if (!ready && s) {
            short want = 0;
            if (events & 1) want |= POLL_IN;
            if (events & 2) want |= POLL_OUT;
            const int ev = want ? WaitSocket(ctx.GetKernel(), s->handle, want, int(timeout)) : 0;
            if (ev > 0) {
                if ((ev & POLL_IN) && (events & 1)) ready |= 1;
                if ((ev & POLL_OUT) && (events & 2)) ready |= 2;
                if ((ev & 8) && (events & 4)) ready |= 4;
            }
        }
        ctx.Push<u32>(ready);
        ctx.SetResult(ready || timeout == 0 ? Result::Success : RESULT_WOULD_BLOCK);
    });
    RegisterCommand(15, "GetVerifyCertError", [this](IpcContext& ctx) { ctx.SetResult(m_verifyError); });
    RegisterCommand(16, "GetNeededServerCertBufferSize", [](IpcContext& ctx) { ctx.Push<u32>(0x10000); ctx.SetResult(Result::Success); });
    RegisterCommand(17, "SetSessionCacheMode", [this](IpcContext& ctx) { m_sessionCacheMode = ctx.Pop<u32>(); ctx.SetResult(Result::Success); });
    RegisterCommand(18, "GetSessionCacheMode", [this](IpcContext& ctx) { ctx.Push<u32>(m_sessionCacheMode); ctx.SetResult(Result::Success); });
    RegisterStub(19, "FlushSessionCache");
    RegisterCommand(20, "SetRenegotiationMode", [this](IpcContext& ctx) { m_renegotiationMode = ctx.Pop<u32>(); ctx.SetResult(Result::Success); });
    RegisterCommand(21, "GetRenegotiationMode", [this](IpcContext& ctx) { ctx.Push<u32>(m_renegotiationMode); ctx.SetResult(Result::Success); });
    // SetOption(u8 valor, u32 opcion)
    RegisterCommand(22, "SetOption", [this](IpcContext& ctx) {
        const u8 value = ctx.Pop<u8>();
        ctx.Pop<u8>(); ctx.Pop<u8>(); ctx.Pop<u8>();
        m_options[ctx.Pop<u32>()] = value != 0;
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(23, "GetOption", [this](IpcContext& ctx) {
        const u32 option = ctx.Pop<u32>();
        ctx.Push<u8>(m_options[option] ? 1 : 0);
        ctx.SetResult(Result::Success);
    });
    // GetVerifyCertErrors -> u32 cuantos, u32 total + lista de resultados
    RegisterCommand(24, "GetVerifyCertErrors", [this](IpcContext& ctx) {
        const u32 n = m_verifyError ? 1 : 0;
        if (n) ctx.WriteBuffer(&m_verifyError, 4, 0);
        ctx.Push<u32>(n);
        ctx.Push<u32>(n);
        ctx.SetResult(Result::Success);
    });
    // GetCipherInfo -> nombre del cifrado (0x40) + version ("TLSv1.2", 0x8)
    RegisterCommand(25, "GetCipherInfo", [this](IpcContext& ctx) {
        char info[0x48] = {};
#ifdef NEXO2_HAS_TLS
        if (m_handshakeDone) {
            std::snprintf(info, 0x40, "%s", mbedtls_ssl_get_ciphersuite(&m_tls->ssl));
            std::snprintf(info + 0x40, 0x8, "%s", mbedtls_ssl_get_version(&m_tls->ssl));
        }
#endif
        ctx.WriteBuffer(info, sizeof(info), 0);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(26, "SetNextAlpnProto", [this](IpcContext& ctx) { m_alpn = ctx.ReadBuffer(0); ctx.SetResult(Result::Success); });
    // GetNextAlpnProto -> u32 estado (1 = acordado), u32 tamano + nombre
    RegisterCommand(27, "GetNextAlpnProto", [this](IpcContext& ctx) {
        u32 state = 0, size = 0;
#ifdef NEXO2_HAS_TLS
        if (m_handshakeDone) {
            if (const char* proto = mbedtls_ssl_get_alpn_protocol(&m_tls->ssl)) {
                const std::string p(proto);
                ctx.WriteBuffer(p.c_str(), p.size() + 1, 0);
                state = 1;
                size = u32(p.size());
            } else if (!m_alpn.empty()) {
                state = 2;   // sin acuerdo
            }
        }
#endif
        ctx.Push<u32>(state);
        ctx.Push<u32>(size);
        ctx.SetResult(Result::Success);
    });
}

// Saludo TLS. Bloqueante: espera al servidor (soltando el candado del kernel). No bloqueante:
// devuelve WOULD_BLOCK y el programa lo vuelve a llamar.
u32 SslConnection::Handshake(IpcContext& ctx) {
#ifdef NEXO2_HAS_TLS
    if (m_handshakeDone) return Result::Success;
    GuestSocket* s = ctx.GetKernel().Network().Get(m_socket);
    if (!s) return RESULT_NO_SOCKET;
    if (!m_tls) {
        m_tls = std::make_unique<TlsSession>();
        if (!m_tls->Setup(m_host, m_verify, *m_context, m_alpn)) {
            m_tls.reset();
            Logger::Log(Logger::Level::Warning, "[ssl] No se pudo preparar mbedTLS");
            return RESULT_CONNECTION_ERROR;
        }
    }
    for (;;) {
        m_tls->sock = s->handle;
        const int ret = mbedtls_ssl_handshake(&m_tls->ssl);
        if (ret == 0) break;
        if (WantsIo(ret)) {
            if (m_ioMode == IO_NON_BLOCKING) return RESULT_WOULD_BLOCK;
            const int ev = WaitSocket(ctx.GetKernel(), s->handle, ret == MBEDTLS_ERR_SSL_WANT_WRITE ? POLL_OUT : POLL_IN,
                                      BLOCKING_TIMEOUT_MS);
            s = ctx.GetKernel().Network().Get(m_socket);
            if (ev <= 0 || !s) return RESULT_CONNECTION_ERROR;
            continue;
        }
        const uint32_t flags = mbedtls_ssl_get_verify_result(&m_tls->ssl);
        Logger::Log(Logger::Level::Info, "[ssl] Saludo con " + m_host + " fallido: " + TlsError(ret));
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || (flags && flags != uint32_t(-1))) {
            m_verifyError = RESULT_CERT_ERROR;
            return RESULT_VERIFY_FAILED;
        }
        return RESULT_CONNECTION_ERROR;
    }
    m_handshakeDone = true;
    Logger::Log(Logger::Level::Info, "[ssl] Conectado a " + m_host + " (" + mbedtls_ssl_get_version(&m_tls->ssl) +
                ", " + mbedtls_ssl_get_ciphersuite(&m_tls->ssl) + ")");
    return Result::Success;
#else
    (void)ctx;
    Logger::Log(Logger::Level::Warning, "[ssl] NeXo se compilo sin mbedTLS (externals/mbedtls): no hay https");
    return RESULT_NOT_AVAILABLE;
#endif
}

// Lee hasta 'max' bytes descifrados. consume = false: Peek (se guardan para el siguiente Read).
u32 SslConnection::ReadSome(IpcContext& ctx, std::vector<u8>& out, size_t max, bool consume) {
    out.clear();
    if (max == 0) return Result::Success;
    if (!m_peek.empty()) {
        const size_t n = std::min(max, m_peek.size());
        out.assign(m_peek.begin(), m_peek.begin() + std::ptrdiff_t(n));
        if (consume) m_peek.erase(m_peek.begin(), m_peek.begin() + std::ptrdiff_t(n));
        return Result::Success;
    }
#ifdef NEXO2_HAS_TLS
    if (!m_handshakeDone) return RESULT_CONNECTION_ERROR;
    GuestSocket* s = ctx.GetKernel().Network().Get(m_socket);
    if (!s) return RESULT_NO_SOCKET;
    out.resize(max);
    for (;;) {
        m_tls->sock = s->handle;
        const int ret = mbedtls_ssl_read(&m_tls->ssl, out.data(), max);
        if (ret >= 0) { out.resize(size_t(ret)); break; }   // 0 = el servidor cerro
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { out.clear(); break; }
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;   // TLS 1.3: aviso, no datos
#endif
        if (WantsIo(ret)) {
            if (m_ioMode == IO_NON_BLOCKING) { out.clear(); return RESULT_WOULD_BLOCK; }
            const int ev = WaitSocket(ctx.GetKernel(), s->handle, ret == MBEDTLS_ERR_SSL_WANT_WRITE ? POLL_OUT : POLL_IN,
                                      BLOCKING_TIMEOUT_MS);
            s = ctx.GetKernel().Network().Get(m_socket);
            if (ev <= 0 || !s) { out.clear(); return RESULT_CONNECTION_ERROR; }
            continue;
        }
        out.clear();
        Logger::Log(Logger::Level::Info, "[ssl] Read: " + TlsError(ret));
        return RESULT_CONNECTION_ERROR;
    }
    if (!consume) m_peek = out;
    return Result::Success;
#else
    (void)ctx;
    return RESULT_NOT_AVAILABLE;
#endif
}

} // namespace NeXo2::HLE
