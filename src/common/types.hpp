#pragma once
#include <cstdint>

// Tipos de datos simplificados (Estilo Kernel/Emulador)
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Direcciones de memoria y offsets
using VAddr = u64; // Dirección Virtual
using PAddr = u64; // Dirección Física