#include "host_certs.hpp"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace NeXo2::HLE::Net {

namespace {
bool AddPemFile(const std::string& path, std::vector<CertBlob>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    CertBlob blob;
    blob.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (blob.data.empty()) return false;
    blob.data.push_back(0);   // mbedTLS quiere el PEM terminado en '\0'
    blob.pem = true;
    out.push_back(std::move(blob));
    return true;
}
}

const std::vector<CertBlob>& SystemRootCerts() {
    static const std::vector<CertBlob> certs = [] {
        std::vector<CertBlob> out;
#ifdef _WIN32
        for (const wchar_t* name : {L"ROOT", L"CA"}) {
            HCERTSTORE store = CertOpenSystemStoreW(0, name);
            if (!store) continue;
            PCCERT_CONTEXT cert = nullptr;
            while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
                if (cert->dwCertEncodingType != X509_ASN_ENCODING) continue;
                CertBlob blob;
                blob.data.assign(cert->pbCertEncoded, cert->pbCertEncoded + cert->cbCertEncoded);
                out.push_back(std::move(blob));
            }
            CertCloseStore(store, 0);
        }
#else
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                 "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"})
            if (AddPemFile(path, out)) break;
#endif
        if (const char* extra = std::getenv("NEXO2_CA_FILE")) AddPemFile(extra, out);
        return out;
    }();
    return certs;
}

} // namespace NeXo2::HLE::Net
