#ifndef INTERPRETER_HPP
#define INTERPRETER_HPP

#include <cstdint>
#include <vector>
#include <array>

class Interpreter {
public:
    Interpreter();
    ~Interpreter();

    void Reset();
    void Step(); // Ejecuta una instrucción

    // Getters para ImGui
    uint64_t GetPC() const { return pc; }
    uint64_t GetX(int index) const { return (index < 31) ? x[index] : 0; }

private:
    std::array<uint64_t, 31> x; // Registros X0-X30
    uint64_t pc;                // Program Counter
    uint64_t sp;                // Stack Pointer
};

#endif