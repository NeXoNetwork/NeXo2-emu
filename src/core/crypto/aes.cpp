#include "aes.hpp"
#include <cstring>
#ifdef NEXO2_HAS_TLS
#include <mbedtls/aes.h>
#endif

namespace NeXo2::Crypto {

#ifdef NEXO2_HAS_TLS
struct Aes128::Impl {
    mbedtls_aes_context enc, dec;
};
Aes128::Aes128(const Key128& key) : m(std::make_unique<Impl>()) {
    mbedtls_aes_init(&m->enc);
    mbedtls_aes_init(&m->dec);
    mbedtls_aes_setkey_enc(&m->enc, key.data(), 128);
    mbedtls_aes_setkey_dec(&m->dec, key.data(), 128);
}
Aes128::~Aes128() { mbedtls_aes_free(&m->enc); mbedtls_aes_free(&m->dec); }
void Aes128::EncryptBlock(const u8 in[16], u8 out[16]) const {
    mbedtls_aes_crypt_ecb(&m->enc, MBEDTLS_AES_ENCRYPT, in, out);
}
void Aes128::DecryptBlock(const u8 in[16], u8 out[16]) const {
    mbedtls_aes_crypt_ecb(&m->dec, MBEDTLS_AES_DECRYPT, in, out);
}
#else
// AES-128 de libro (FIPS-197), byte a byte
namespace {
const u8 SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
u8 INV[256];
struct InitInv { InitInv() { for (int i = 0; i < 256; ++i) INV[SBOX[i]] = u8(i); } } init_inv;
inline u8 Xt(u8 x) { return u8((x << 1) ^ ((x >> 7) * 0x1B)); }
inline u8 Mul(u8 a, u8 b) {
    u8 r = 0;
    while (b) { if (b & 1) r ^= a; a = Xt(a); b >>= 1; }
    return r;
}
} // namespace

struct Aes128::Impl { u8 rk[176]; };

Aes128::Aes128(const Key128& key) : m(std::make_unique<Impl>()) {
    std::memcpy(m->rk, key.data(), 16);
    u8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        u8 t[4] = {m->rk[i - 4], m->rk[i - 3], m->rk[i - 2], m->rk[i - 1]};
        if (i % 16 == 0) {
            const u8 x = t[0];
            t[0] = u8(SBOX[t[1]] ^ rcon); t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[x];
            rcon = Xt(rcon);
        }
        for (int j = 0; j < 4; ++j) m->rk[i + j] = u8(m->rk[i - 16 + j] ^ t[j]);
    }
}
Aes128::~Aes128() = default;

void Aes128::EncryptBlock(const u8 in[16], u8 out[16]) const {
    u8 s[16];
    for (int i = 0; i < 16; ++i) s[i] = u8(in[i] ^ m->rk[i]);
    for (int round = 1; round <= 10; ++round) {
        u8 t[16];
        for (int i = 0; i < 16; ++i) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];   // SubBytes + ShiftRows
        if (round < 10) {
            for (int c = 0; c < 4; ++c) {
                u8* col = t + 4 * c;
                const u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = u8(Xt(a0) ^ Xt(a1) ^ a1 ^ a2 ^ a3);
                col[1] = u8(a0 ^ Xt(a1) ^ Xt(a2) ^ a2 ^ a3);
                col[2] = u8(a0 ^ a1 ^ Xt(a2) ^ Xt(a3) ^ a3);
                col[3] = u8(Xt(a0) ^ a0 ^ a1 ^ a2 ^ Xt(a3));
            }
        }
        for (int i = 0; i < 16; ++i) s[i] = u8(t[i] ^ m->rk[16 * round + i]);
    }
    std::memcpy(out, s, 16);
}

void Aes128::DecryptBlock(const u8 in[16], u8 out[16]) const {
    u8 s[16];
    for (int i = 0; i < 16; ++i) s[i] = u8(in[i] ^ m->rk[160 + i]);
    for (int round = 9; round >= 0; --round) {
        u8 t[16];
        for (int i = 0; i < 16; ++i) t[(i + 4 * (i % 4)) % 16] = INV[s[i]];   // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; ++i) t[i] ^= m->rk[16 * round + i];
        if (round > 0) {
            for (int c = 0; c < 4; ++c) {
                u8* col = t + 4 * c;
                const u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = u8(Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9));
                col[1] = u8(Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13));
                col[2] = u8(Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11));
                col[3] = u8(Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14));
            }
        }
        std::memcpy(s, t, 16);
    }
    std::memcpy(out, s, 16);
}
#endif

void Aes128::DecryptEcb(u8* data, size_t size) const {
    for (size_t i = 0; i + 16 <= size; i += 16) DecryptBlock(data + i, data + i);
}

void AesCtrCrypt(const Aes128& aes, const u8 ctr_hi[8], u64 offset, u8* data, size_t size) {
    u64 block = offset / 16;
    size_t skip = size_t(offset % 16), done = 0;
    while (done < size) {
        u8 ctr[16], ks[16];
        std::memcpy(ctr, ctr_hi, 8);
        for (int i = 0; i < 8; ++i) ctr[15 - i] = u8(block >> (8 * i));
        aes.EncryptBlock(ctr, ks);
        for (size_t i = skip; i < 16 && done < size; ++i) data[done++] ^= ks[i];
        skip = 0;
        ++block;
    }
}

namespace {
void Xts(const Aes128& data_key, const Aes128& tweak_key, u8* data, size_t size, u64 first_sector,
         size_t sector_size, bool decrypt) {
    for (size_t off = 0; off + sector_size <= size; off += sector_size) {
        u8 tweak[16] = {};
        const u64 sector = first_sector + off / sector_size;
        for (int i = 0; i < 8; ++i) tweak[15 - i] = u8(sector >> (8 * i));   // big endian
        tweak_key.EncryptBlock(tweak, tweak);
        for (size_t b = 0; b < sector_size; b += 16) {
            u8* p = data + off + b;
            for (int i = 0; i < 16; ++i) p[i] ^= tweak[i];
            if (decrypt) data_key.DecryptBlock(p, p); else data_key.EncryptBlock(p, p);
            for (int i = 0; i < 16; ++i) p[i] ^= tweak[i];
            // tweak *= x en GF(2^128) (little endian, como XTS)
            u8 carry = 0;
            for (int i = 0; i < 16; ++i) {
                const u8 next = u8(tweak[i] >> 7);
                tweak[i] = u8((tweak[i] << 1) | carry);
                carry = next;
            }
            if (carry) tweak[0] ^= 0x87;
        }
    }
}
} // namespace

void AesXtsDecrypt(const Aes128& data_key, const Aes128& tweak_key, u8* data, size_t size,
                   u64 first_sector, size_t sector_size) {
    Xts(data_key, tweak_key, data, size, first_sector, sector_size, true);
}
void AesXtsEncrypt(const Aes128& data_key, const Aes128& tweak_key, u8* data, size_t size,
                   u64 first_sector, size_t sector_size) {
    Xts(data_key, tweak_key, data, size, first_sector, sector_size, false);
}

} // namespace NeXo2::Crypto
