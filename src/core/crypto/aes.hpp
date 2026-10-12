#pragma once
// AES-128 para leer los archivos de los juegos (NCA): ECB (claves), CTR (secciones) y XTS
// (cabecera de la NCA, con el numero de sector en big endian como hace Nintendo).
// Con mbedTLS compilado se usa el suyo (instrucciones AES del procesador); si no, uno propio.
#include <array>
#include <memory>
#include "common/types.hpp"

namespace NeXo2::Crypto {

using Key128 = std::array<u8, 16>;

class Aes128 {
public:
    explicit Aes128(const Key128& key);
    ~Aes128();
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;
    void EncryptBlock(const u8 in[16], u8 out[16]) const;
    void DecryptBlock(const u8 in[16], u8 out[16]) const;
    void DecryptEcb(u8* data, size_t size) const;   // 'size' multiplo de 16
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// CTR: descifra (o cifra, es lo mismo) 'size' bytes que empiezan en 'offset' dentro de una
// zona cuyo contador inicial es 'ctr' (los 8 bytes bajos se ponen a offset / 16, big endian).
void AesCtrCrypt(const Aes128& aes, const u8 ctr_hi[8], u64 offset, u8* data, size_t size);

// XTS de Nintendo: sectores de 'sector_size' bytes, el primero es 'first_sector'
void AesXtsDecrypt(const Aes128& data_key, const Aes128& tweak_key, u8* data, size_t size,
                   u64 first_sector, size_t sector_size);

// Lo contrario (para los tests, que fabrican NCA de prueba)
void AesXtsEncrypt(const Aes128& data_key, const Aes128& tweak_key, u8* data, size_t size,
                   u64 first_sector, size_t sector_size);

} // namespace NeXo2::Crypto
