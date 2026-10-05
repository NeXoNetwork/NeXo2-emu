// Instrucciones de criptografia de ARMv8: AES (AESE, AESD, AESMC, AESIMC),
// SHA1 (SHA1C/P/M/H/SU0/SU1) y SHA256 (SHA256H/H2/SU0/SU1).
// Siguen el pseudocodigo del manual de ARM (shared/functions/crypto).
// Las de CRC32 estan en interpreter_dp_reg.cpp (son instrucciones de enteros).
#include "simd_common.hpp"

namespace NeXo2::Core {

using Common::Bit;
using Common::Bits;

namespace {

// --- AES ---

// Multiplicar en GF(2^8) con el polinomio de AES (x^8 + x^4 + x^3 + x + 1)
u8 GfMul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = u8((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
        b >>= 1;
    }
    return r;
}

// Las S-box se calculan una vez (inverso en GF(2^8) + transformacion afin)
// en lugar de escribir las tablas de 256 valores a mano.
struct AesTables {
    u8 sbox[256];
    u8 inv[256];
    AesTables() {
        for (unsigned i = 0; i < 256; ++i) {
            u8 x = 0;   // inverso multiplicativo (el de 0 es 0)
            if (i) for (unsigned j = 1; j < 256; ++j) if (GfMul(u8(i), u8(j)) == 1) { x = u8(j); break; }
            u8 s = x;
            for (unsigned k = 1; k <= 4; ++k) s ^= u8((x << k) | (x >> (8 - k)));
            s ^= 0x63;
            sbox[i] = s;
            inv[s] = u8(i);
        }
    }
};
const AesTables& Aes() {
    static const AesTables t;
    return t;
}

using Block = std::array<u8, 16>;   // byte i = bits 8i..8i+7 del registro; columna c = bytes 4c..4c+3

Block ToBlock(const V128& v) { Block b; for (unsigned i = 0; i < 16; ++i) b[i] = u8(v.Get(i, 1)); return b; }
V128  FromBlock(const Block& b) { V128 v; for (unsigned i = 0; i < 16; ++i) v.Set(i, 1, b[i]); return v; }

Block MixColumns(const Block& in, bool inverse) {
    Block out;
    // Coeficientes de la matriz: {2,3,1,1} o, al deshacer, {14,11,13,9}
    const u8 c0 = inverse ? 14 : 2, c1 = inverse ? 11 : 3, c2 = inverse ? 13 : 1, c3 = inverse ? 9 : 1;
    for (unsigned c = 0; c < 4; ++c) {
        const u8* a = &in[4 * c];
        for (unsigned r = 0; r < 4; ++r)
            out[4 * c + r] = u8(GfMul(a[r], c0) ^ GfMul(a[(r + 1) % 4], c1) ^ GfMul(a[(r + 2) % 4], c2) ^ GfMul(a[(r + 3) % 4], c3));
    }
    return out;
}

// --- SHA ---
inline u32 Rol(u32 x, unsigned n) { return (x << n) | (x >> (32 - n)); }
inline u32 Ror(u32 x, unsigned n) { return (x >> n) | (x << (32 - n)); }
inline u32 Choose(u32 x, u32 y, u32 z)   { return ((y ^ z) & x) ^ z; }
inline u32 Parity(u32 x, u32 y, u32 z)   { return x ^ y ^ z; }
inline u32 Majority(u32 x, u32 y, u32 z) { return (x & y) | ((x | y) & z); }

using Words = std::array<u32, 4>;   // palabra e = bits 32e..32e+31
Words ToWords(const V128& v) { return {u32(v.lo), u32(v.lo >> 32), u32(v.hi), u32(v.hi >> 32)}; }
V128  FromWords(const Words& w) { V128 v; v.lo = w[0] | (u64(w[1]) << 32); v.hi = w[2] | (u64(w[3]) << 32); return v; }

// SHA256hash del manual: X y Y son las dos mitades del estado (a..d y e..h)
Words Sha256Hash(Words x, Words y, const Words& w, bool part1) {
    for (unsigned e = 0; e < 4; ++e) {
        const u32 chs = Choose(y[0], y[1], y[2]);
        const u32 maj = Majority(x[0], x[1], x[2]);
        const u32 t = y[3] + (Ror(y[0], 6) ^ Ror(y[0], 11) ^ Ror(y[0], 25)) + chs + w[e];
        x[3] = t + x[3];
        y[3] = t + (Ror(x[0], 2) ^ Ror(x[0], 13) ^ Ror(x[0], 22)) + maj;
        // <Y, X> = ROL(Y:X, 32)
        const u32 xt = x[3], yt = y[3];
        x = {yt, x[0], x[1], x[2]};
        y = {xt, y[0], y[1], y[2]};
    }
    return part1 ? x : y;
}

} // namespace

bool Interpreter::ExecCrypto(u32 instr) {
    const unsigned rd = Bits(instr, 0, 5), rn = Bits(instr, 5, 5), rm = Bits(instr, 16, 5);

    // AES: 0100 1110 0010 1000 0 opcode(5) 10 Rn Rd
    if ((instr & 0xFFFE0C00) == 0x4E280800) {
        const u32 opcode = Bits(instr, 12, 5);
        const AesTables& t = Aes();
        Block in = ToBlock(Vreg(rd)), out{};
        switch (opcode) {
            case 0b00100:   // AESE: AddRoundKey, ShiftRows, SubBytes
            case 0b00101: { // AESD: AddRoundKey, InvShiftRows, InvSubBytes
                const Block key = ToBlock(Vreg(rn));
                for (unsigned i = 0; i < 16; ++i) in[i] ^= key[i];
                const bool dec = opcode == 0b00101;
                for (unsigned c = 0; c < 4; ++c)
                    for (unsigned r = 0; r < 4; ++r) {
                        if (!dec) out[r + 4 * c] = t.sbox[in[r + 4 * ((c + r) % 4)]];
                        else      out[r + 4 * ((c + r) % 4)] = t.inv[in[r + 4 * c]];
                    }
                break;
            }
            case 0b00110: out = MixColumns(ToBlock(Vreg(rn)), false); break;   // AESMC
            case 0b00111: out = MixColumns(ToBlock(Vreg(rn)), true);  break;   // AESIMC
            default: return false;
        }
        Vreg(rd) = FromBlock(out);
        return true;
    }

    // SHA de dos registros: 0101 1110 0010 1000 0 opcode(5) 10 Rn Rd
    if ((instr & 0xFFFE0C00) == 0x5E280800) {
        const u32 opcode = Bits(instr, 12, 5);
        const Words d = ToWords(Vreg(rd)), n = ToWords(Vreg(rn));
        switch (opcode) {
            case 0b00000:   // SHA1H: ROL(Sn, 30)
                SetVScalar(rd, Rol(n[0], 30), 4);
                return true;
            case 0b00001: { // SHA1SU1
                const Words tt = {d[0] ^ n[1], d[1] ^ n[2], d[2] ^ n[3], d[3]};
                const Words w = {Rol(tt[0], 1), Rol(tt[1], 1), Rol(tt[2], 1), Rol(tt[3], 1) ^ Rol(tt[0], 2)};
                Vreg(rd) = FromWords(w);
                return true;
            }
            case 0b00010: { // SHA256SU0
                const Words tt = {d[1], d[2], d[3], n[0]};
                Words r;
                for (unsigned e = 0; e < 4; ++e) r[e] = (Ror(tt[e], 7) ^ Ror(tt[e], 18) ^ (tt[e] >> 3)) + d[e];
                Vreg(rd) = FromWords(r);
                return true;
            }
            default: return false;
        }
    }

    // SHA de tres registros: 0101 1110 000 Rm 0 opcode(3) 00 Rn Rd
    if ((instr & 0xFFE08C00) == 0x5E000000) {
        const u32 opcode = Bits(instr, 12, 3);
        const Words d = ToWords(Vreg(rd)), n = ToWords(Vreg(rn)), m = ToWords(Vreg(rm));
        switch (opcode) {
            case 0b000: case 0b001: case 0b010: {   // SHA1C / SHA1P / SHA1M
                Words x = d;
                u32 y = n[0];
                for (unsigned e = 0; e < 4; ++e) {
                    const u32 f = opcode == 0 ? Choose(x[1], x[2], x[3])
                                : opcode == 1 ? Parity(x[1], x[2], x[3])
                                              : Majority(x[1], x[2], x[3]);
                    y = y + Rol(x[0], 5) + f + m[e];
                    x[1] = Rol(x[1], 30);
                    // <Y, X> = ROL(Y:X, 32)
                    const u32 top = x[3];
                    x = {y, x[0], x[1], x[2]};
                    y = top;
                }
                Vreg(rd) = FromWords(x);
                return true;
            }
            case 0b011: {   // SHA1SU0
                const Words r = {d[2] ^ d[0] ^ m[0], d[3] ^ d[1] ^ m[1], n[0] ^ d[2] ^ m[2], n[1] ^ d[3] ^ m[3]};
                Vreg(rd) = FromWords(r);
                return true;
            }
            case 0b100: Vreg(rd) = FromWords(Sha256Hash(d, n, m, true));  return true;   // SHA256H
            case 0b101: Vreg(rd) = FromWords(Sha256Hash(n, d, m, false)); return true;   // SHA256H2
            case 0b110: {   // SHA256SU1
                auto sig1 = [](u32 v) { return Ror(v, 17) ^ Ror(v, 19) ^ (v >> 10); };
                const Words t0 = {n[1], n[2], n[3], m[0]};
                Words r;
                r[0] = sig1(m[2]) + d[0] + t0[0];
                r[1] = sig1(m[3]) + d[1] + t0[1];
                r[2] = sig1(r[0]) + d[2] + t0[2];
                r[3] = sig1(r[1]) + d[3] + t0[3];
                Vreg(rd) = FromWords(r);
                return true;
            }
            default: return false;
        }
    }
    return false;
}

} // namespace NeXo2::Core
