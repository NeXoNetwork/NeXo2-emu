// Funciones en C normales. Se compilan para ARM64 con clang y el binario
// resultante se ejecuta en el interprete (ver tools/asm2cpp.py).
// Asi comprobamos que la CPU entiende codigo "real" generado por un compilador.
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned int u32;

u64 fibonacci(u32 n) {
    u64 a = 0, b = 1;
    for (u32 i = 0; i < n; ++i) { u64 t = a + b; a = b; b = t; }
    return a;
}

u64 factorial_rec(u64 n) {
    return n <= 1 ? 1 : n * factorial_rec(n - 1);
}

u64 string_length(const char* s) {
    u64 n = 0;
    while (s[n]) ++n;
    return n;
}

void bubble_sort(s64* v, u32 count) {
    for (u32 i = 0; i < count; ++i)
        for (u32 j = 0; j + 1 < count - i; ++j)
            if (v[j] > v[j + 1]) { s64 t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; }
}

u64 gcd(u64 a, u64 b) {
    while (b) { u64 t = a % b; a = b; b = t; }
    return a;
}

u32 count_primes(u32 limit) {
    u32 count = 0;
    for (u32 n = 2; n < limit; ++n) {
        u32 is_prime = 1;
        for (u32 d = 2; d * d <= n; ++d)
            if (n % d == 0) { is_prime = 0; break; }
        count += is_prime;
    }
    return count;
}

// FNV-1a: hash tipico con multiplicaciones y XOR
u32 hash_fnv1a(const unsigned char* data, u64 size) {
    u32 h = 2166136261u;
    for (u64 i = 0; i < size; ++i) { h ^= data[i]; h *= 16777619u; }
    return h;
}
