#pragma once
// Traductor de shaders Maxwell -> SPIR-V (GPU fase 3b).
//
// Convierte el codigo de un ShaderProgram (el mismo que ejecuta el interprete) en un modulo
// SPIR-V que Vulkan puede ejecutar en la tarjeta grafica. Hace lo mismo que el interprete,
// instruccion a instruccion, para que los resultados coincidan.
//
// El codigo de Maxwell tiene saltos libres (BRA, y SSY/SYNC, PBK/BRK, CAL/RET con una pila),
// pero SPIR-V solo admite bucles e ifs bien anidados. Por eso el programa se traduce como un
// "bucle con un switch": una variable 'pc' dice que bloque toca y cada bloque deja en 'pc'
// el siguiente. Los drivers lo optimizan bien.
//
// Ver docs/07-nexo-internals/gpu-vulkan.md.
#include <array>
#include <string>
#include <vector>
#include "common/types.hpp"
#include "shader.hpp"

namespace NeXo2::GPU {

struct SpirvOptions {
    enum class Stage {
        ComputeTest,   // para los tests: atributos y constbufs en storage buffers (ver abajo)
        Vertex,
        Fragment,
    };
    Stage stage = Stage::ComputeTest;
    bool int64 = true;   // la GPU tiene enteros de 64 bits (shaderInt64)

    // --- Solo Fragment ---
    u32 color_outputs = 1;        // render targets enlazados: salidas en las locations 0..n-1
    bool mrt = true;              // false: todos los render targets reciben la salida 0
    std::array<u8, 8> color_int{}; // 0 = float, 1 = entero (los bits del registro tal cual)
};

// Enlaces (descriptor set 0)
namespace SpirvBinding {
    // c[i] -> binding i (storage buffer de uint), solo los que use el shader
    constexpr u32 DRAW_INFO = 18;  // Fragment: uint[]: [0] = SR_Y_DIRECTION (float),
                                   // [4 + 2k], [5 + 2k] = escala de las coordenadas de la textura k
                                   // (1 normalizadas; 1/ancho, 1/alto si el sampler usa texeles)
    constexpr u32 ATTR_IN = 20;    // ComputeTest: uint[256]: a[0x000..0x3FF] que lee ALD/IPA
    constexpr u32 ATTR_OUT = 21;   // ComputeTest: uint[256]: lo que escribe AST
    constexpr u32 TEXTURE0 = 32;   // Fragment: textura k (sampler combinado) en el binding 32 + k
}

// Entrada del shader de pixeles: un vec4 de atributos, a[slot * 16 .. slot * 16 + 15]
struct SpirvVarying {
    enum Kind : u8 {
        Linear = 0,        // IPA lineal: el valor, interpolado en pantalla (NoPerspective)
        Perspective = 1,   // IPA con perspectiva: el valor con correccion de perspectiva; el
                           // shader lo multiplica por gl_FragCoord.w = valor / w interpolado en
                           // pantalla, que es lo que da IPA (el programa multiplica luego por w)
        Flat = 2,          // constante: el del vertice que manda (uvec4, los bits tal cual)
    };
    u8 slot = 0;
    Kind kind = Linear;
};
// Textura que usa el shader de pixeles: handle en c[TextureConstbuf][slot * 4]
struct SpirvTexture {
    u32 slot = 0;
    u8 dim = 1;          // 0 1D, 1 2D, 2 3D, 3 cubo
    bool array = false;
};

struct SpirvResult {
    bool ok = false;
    std::string error;          // por que no se pudo traducir (se usa el dibujo por software)
    std::vector<u32> code;      // el modulo SPIR-V
    u32 cbuf_mask = 0;          // constbufs que usa (bit i = c[i])
    u32 instructions = 0;       // instrucciones Maxwell traducidas
    u32 code_end = 0;           // bytes del programa que se han leido (cabecera incluida)
    std::vector<SpirvVarying> varyings;   // Fragment: location i = varyings[i]
    std::vector<SpirvTexture> textures;   // Fragment: binding TEXTURE0 + i = textures[i]
    bool writes_depth = false;
};

SpirvResult TranslateToSpirv(ShaderProgram& program, const SpirvOptions& options);

// Shader de vertices "de paso" para el dibujo con Vulkan: el shader de vertices de Maxwell se
// ejecuta en la CPU y la GPU solo recibe sus resultados. Entradas: location 0 = posicion
// (ya transformada), location 1 = distancias a los planos cercano y lejano (gl_ClipDistance),
// location 2 + i = varyings[i]. Salidas: gl_Position, gl_ClipDistance[2] y location i.
std::vector<u32> BuildPassthroughVertexShader(const std::vector<SpirvVarying>& varyings);

} // namespace NeXo2::GPU
