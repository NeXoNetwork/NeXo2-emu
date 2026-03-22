#pragma once
#include <vector>
#include <cstdint>
#include <iostream>

class Memory {
public:
    // 12 GB de RAM = 12LL * 1024 * 1024 * 1024
    static const uint64_t RAM_SIZE = 12LL * 1024 * 1024 * 1024; 

    Memory() {
        // Reservamos los 12GB de forma contigua
        ram.resize(RAM_SIZE, 0);
        std::cout << "Memoria NeXo 2: 12GB RAM inicializada." << std::endl;
    }

    // Leer 8, 16, 32, 64 bits
    template<typename T>
    T Read(uint64_t addr) {
        if (addr + sizeof(T) > RAM_SIZE) return 0;
        return *reinterpret_cast<T*>(&ram[addr]);
    }

    // Escribir
    template<typename T>
    void Write(uint64_t addr, T value) {
        if (addr + sizeof(T) > RAM_SIZE) return;
        *reinterpret_cast<T*>(&ram[addr]) = value;
    }

    bool IsReady() const { return !ram.empty(); }

private:
    std::vector<uint8_t> ram;
};