#include <iostream>
#include <iomanip>
#include "core/memory/memory.hpp"
#include "core/arm64/interpreter.hpp"

using namespace NeXo2::Core;

void RunTest() {
    Memory ram;
    Interpreter cpu(ram);

    std::cout << "--- [NeXo 2] Starting CPU Unit Test ---" << std::endl;

    // 1. Configurar estado inicial de los registros
    // Vamos a simular que X1 = 10 y X2 = 20
    // (Tendrías que hacer CPUState público o añadir un setter en Interpreter)
    
    // 2. Inyectar instrucción ARM64 en la RAM (Dirección 0)
    // Instrucción: ADD X0, X1, X2 -> 0x8B020020
    ram.Write<uint32_t>(0, 0x8B020020);
    
    std::cout << "[Test] Injected ADD X0, X1, X2 at address 0x0" << std::endl;

    // 3. Ejecutar un paso (Step)
    cpu.Step();

    std::cout << "[Test] Instruction executed." << std::endl;
    std::cout << "--- Test Finished ---" << std::endl;
}

int main() {
    try {
        RunTest();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with error: " << e.what() << std::endl;
    }
    return 0;
}