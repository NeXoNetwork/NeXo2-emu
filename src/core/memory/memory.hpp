#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

namespace NeXo2::Core {

class Memory {
public:
    Memory() {
        m_data.resize(0x10000, 0); // 64KB de RAM para pruebas
    }

    template <typename T>
    T Read(uint64_t address) {
        T value;
        if (address + sizeof(T) <= m_data.size()) {
            std::memcpy(&value, &m_data[address], sizeof(T));
        } else {
            value = 0;
        }
        return value;
    }

    template <typename T>
    void Write(uint64_t address, T value) {
        if (address + sizeof(T) <= m_data.size()) {
            std::memcpy(&m_data[address], &value, sizeof(T));
        }
    }

private:
    std::vector<uint8_t> m_data;
};

} // namespace NeXo2::Core