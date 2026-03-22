#include "interpreter.hpp"
#include <iostream>

Interpreter::Interpreter() {
    Reset();
}

Interpreter::~Interpreter() {
}

void Interpreter::Reset() {
    // Inicializamos registros a 0
    for (auto& reg : x) {
        reg = 0;
    }
    
    // Dirección de inicio típica (ejemplo)
    pc = 0x80000000;
    sp = 0x0;
    
    std::cout << "[CPU] Core Reset: PC set to 0x" << std::hex << pc << std::dec << std::endl;
}

void Interpreter::Step() {
    // Aquí irá el Fetch -> Decode -> Execute
    // Por ahora, simulamos que avanza una instrucción (4 bytes en ARM64)
    pc += 4;
}