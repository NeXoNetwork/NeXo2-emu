// Tests de la memoria emulada (tabla de paginas de dos niveles).
#include "test_framework.hpp"
#include "memory.hpp"

using namespace NeXo2;
using Core::Memory;

TEST(Memory_ReadWriteAndPageBorders) {
    Memory mem;
    // Sin escribir nada todo lee 0 y no se crea ninguna pagina
    CHECK_EQ(mem.Read<u64>(0x80000000), 0);
    CHECK_EQ(mem.AllocatedPages(), 0);

    mem.Write<u32>(0x80000010, 0x12345678);
    CHECK_EQ(mem.Read<u32>(0x80000010), 0x12345678);
    CHECK_EQ(mem.Read<u8>(0x80000011), 0x56);          // little endian
    CHECK_EQ(mem.AllocatedPages(), 1);

    // Un u64 que cruza el borde entre dos paginas (4 bytes en cada una)
    const u64 border = 0x80001000 - 4;
    mem.Write<u64>(border, 0x1122334455667788ull);
    CHECK_EQ(mem.Read<u64>(border), 0x1122334455667788ull);
    CHECK_EQ(mem.Read<u32>(0x80001000), 0x11223344);
    CHECK_EQ(mem.AllocatedPages(), 2);

    // Dos paginas en zonas de 2 MB distintas (dos tablas de nivel 2)
    mem.Write<u16>(0x1FFFF0000, 0xBEEF);
    CHECK_EQ(mem.Read<u16>(0x1FFFF0000), 0xBEEF);
    CHECK(mem.PagePointer(0x1FFFF0004) != nullptr);
    CHECK(mem.PagePointer(0x1FFFE0000) == nullptr);

    // Fuera del espacio de direcciones: se lee 0 y la escritura se ignora
    mem.Write<u32>(Memory::ADDRESS_SPACE + 8, 1);
    CHECK_EQ(mem.Read<u32>(Memory::ADDRESS_SPACE + 8), 0);
    CHECK(mem.PagePointer(Memory::ADDRESS_SPACE) == nullptr);

    // Clear borra todo y cambia la generacion
    const u64 gen = mem.Generation();
    mem.Clear();
    CHECK_EQ(mem.AllocatedPages(), 0);
    CHECK_EQ(mem.Read<u32>(0x80000010), 0);
    CHECK(mem.Generation() != gen);
}
