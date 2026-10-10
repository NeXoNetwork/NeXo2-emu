#pragma once
// Shaders de la GPU Maxwell (SM 5.x, el juego de instrucciones de la GPU de la Switch).
//
// Un shader en memoria es:
//   +0x00  cabecera SPH (0x50 bytes): tipo de shader, atributos que lee/escribe...
//   +0x50  codigo: grupos de 32 bytes = 1 palabra de "planificacion" + 3 instrucciones
//          de 64 bits. La palabra de planificacion (esperas, barreras) no cambia el
//          resultado, asi que nosotros la saltamos.
//
// Este interprete ejecuta UN hilo (un vertice o un pixel) cada vez. La GPU real ejecuta
// 32 a la vez ("warp") y por eso tiene instrucciones para reunir hilos que se separan
// (SSY/SYNC, PBK/BRK...); con un solo hilo bastan una pila de direcciones.
//
// Fuentes de la codificacion: el desensamblador de envytools (envydis, gm107) y el
// codificador de mesa (nouveau, codegen gm107), ambos con licencia MIT. Ver
// docs/07-nexo-internals/gpu-shaders.md.
#include <array>
#include <functional>
#include <string>
#include <vector>
#include "common/types.hpp"

namespace NeXo2::GPU {

// ----------------------------------------------------------------------------
// Instrucciones
// ----------------------------------------------------------------------------
enum class ShOp : u8 {
    Invalid,
    // Control de flujo
    Exit, Bra, Jmp, Ssy, Sync, Pbk, Brk, Pcnt, Cont, Cal, Ret, Kil, Nop,
    // Movimientos y conversiones
    Mov, Mov32i, S2R, Sel, F2F, F2I, I2F, I2I,
    // Coma flotante
    Fadd, Fadd32i, Fmul, Fmul32i, Ffma, Ffma32i, Mufu, Fmnmx, Rro, Fset, Fsetp, Fcmp,
    // Enteros
    Lop, Lop32i, Iadd, Iadd32i, Imul, Imul32i, Imad, Iscadd, Xmad, Imnmx, Iset, Isetp, Icmp,
    Shl, Shr, Popc, Bfi, Bfe, Flo,
    // Memoria y atributos
    Ald, Ast, Ipa, Ldc, Ldl, Stl,
    // Texturas
    Tex, Texs, Tlds, Txd, Txq,
    Count
};

// De donde sale el operando B (y C en las instrucciones de 3 operandos)
enum class ShForm : u8 {
    None,     // sin operando B
    Reg,      // B = registro (bits 20..27), C = registro (bits 39..46)
    Cbuf,     // B = c[indice][offset], C = registro (39)
    Imm,      // B = inmediato de 19 bits + signo (bit 56), C = registro (39)
    CbufC,    // B = registro (39), C = c[indice][offset]   (formas 0x51/0x52/0x53)
    Imm32,    // B = inmediato de 32 bits (bits 20..51)
};

struct ShaderInstr {
    u64 raw = 0;
    ShOp op = ShOp::Invalid;
    ShForm form = ShForm::None;
    u8 pred = 7;            // predicado que la protege (7 = PT, siempre cierto)
    bool pred_neg = false;
    u8 rd = 0xFF, ra = 0xFF;
    u8 rb = 0xFF;           // registro B (formas Reg y CbufC)
    u8 rc = 0xFF;           // registro C (39..46)
    u8 cb_index = 0;        // constbuf (formas Cbuf y CbufC)
    u32 cb_offset = 0;      // en bytes
    u32 imm = 0;            // inmediato ya preparado (Imm, Imm32)

    // Campo de 'len' bits en la posicion 'pos'
    u32 Bits(u32 pos, u32 len) const { return u32((raw >> pos) & ((1ull << len) - 1)); }
    bool Bit(u32 pos) const { return (raw >> pos) & 1; }
};

// Decodifica una instruccion. 'Invalid' si no se conoce.
ShaderInstr DecodeShaderInstr(u64 raw);
// Nombre corto (para mensajes): "ffma", "ipa"...
const char* ShOpName(ShOp op);

// ----------------------------------------------------------------------------
// Cabecera SPH (lo que nos interesa)
// ----------------------------------------------------------------------------
struct ShaderHeader {
    u32 words[20] = {};
    u32 SphType() const { return words[0] & 0x1F; }            // 1 = VTG, 2 = PS
    u32 ShaderType() const { return (words[0] >> 10) & 0xF; }  // 1 = vertice, 5 = pixel
    bool KillsPixels() const { return (words[0] >> 15) & 1; }
    // Pixel: componentes que escribe en cada render target (4 bits por target)
    u32 OmapTarget() const { return words[18]; }
    bool OmapSampleMask() const { return words[19] & 1; }
    bool OmapDepth() const { return (words[19] >> 1) & 1; }
};

// Peticion de muestreo desde un shader
struct TextureRequest {
    float coords[3] = {};        // u, v, w (3D/cubo) en float
    s32 icoords[3] = {};         // texelFetch: coordenadas enteras
    u32 layer = 0;               // arrays
    float lod = 0;               // lod explicito (o sesgo si 'bias')
    bool explicit_lod = false;   // .LL / .LZ
    bool bias = false;           // .LB
    s32 offset[3] = {};          // textureOffset
    bool fetch = false;          // texelFetch (TLD/TLDS): sin filtro ni repeticion
    bool depth_compare = false;  // muestreadores "shadow"
    float dref = 0;
    bool grad = false;           // textureGrad (TXD): derivadas de u, v, w en x y en y
    float ddx[3] = {}, ddy[3] = {};
};

// ----------------------------------------------------------------------------
// Lo que el shader necesita de fuera (atributos, constantes, texturas)
// ----------------------------------------------------------------------------
class ShaderEnv {
public:
    virtual ~ShaderEnv() = default;
    // c[index][offset]
    virtual u32 ReadConst(u32 index, u32 offset) = 0;
    // Atajo (opcional): si cb_direct, los constbufs ya estan en memoria y el interprete los
    // lee de aqui sin llamar a ReadConst (se lee muchas veces por pixel).
    bool cb_direct = false;
    const u32* cb_data[18] = {};
    u32 cb_words[18] = {};
    u32 Const(u32 index, u32 offset) {
        if (cb_direct) return index < 18 && offset / 4 < cb_words[index] ? cb_data[index][offset / 4] : 0;
        return ReadConst(index, offset);
    }
    // Atributos: a[addr]. En un shader de vertices: entradas (posicion, color...) y
    // valores del sistema (VertexID...). AST escribe las salidas.
    virtual u32 ReadAttribute(u32 addr) = 0;
    virtual void WriteAttribute(u32 addr, u32 value) = 0;
    // IPA (solo pixel): atributo interpolado en el pixel. mode: 0 lineal, 1 perspectiva
    // (el shader luego multiplica por w), 2 constante (flat), 3 "sc"
    virtual float Interpolate(u32 addr, u32 mode) = 0;
    // Registro especial (S2R): id del hilo, etc.
    virtual u32 SystemRegister(u32 sr) { (void)sr; return 0; }
    // Texturas: handle (bits 0..19 indice de imagen, 20..31 indice de sampler) ->
    // 4 valores de 32 bits (floats o enteros segun el formato)
    virtual void SampleTexture(u32 handle, const TextureRequest& req, u32 out[4]) {
        (void)handle; (void)req; out[0] = out[1] = out[2] = 0; out[3] = 0x3F800000u; }
    // textureSize / textureQueryLevels: ancho, alto, profundidad o capas, niveles
    virtual void QueryTexture(u32 handle, u32 lod, u32 out[4]) {
        (void)handle; (void)lod; out[0] = out[1] = out[2] = out[3] = 0; }
    // Indice del constbuf con los handles de texturas (SetBindlessTexture)
    virtual u32 TextureConstbuf() { return 0; }
};

// ----------------------------------------------------------------------------
// Programa decodificado (se decodifica al vuelo, la primera vez que se ejecuta cada parte)
// ----------------------------------------------------------------------------
class ShaderProgram {
public:
    // 'read' lee 'size' bytes de la memoria de la GPU en 'addr'
    using ReadFn = std::function<void(u64 addr, void* dst, size_t size)>;
    ShaderProgram(u64 address, ReadFn read);

    const ShaderHeader& Header() const { return m_header; }
    u64 Address() const { return m_address; }
    // Instruccion en el offset (en bytes, desde el inicio del codigo = address + 0x50)
    // Instruccion en 'offset' (decodificada la primera vez). Camino rapido en linea: es lo
    // que mas se llama (una vez por instruccion y pixel).
    const ShaderInstr& At(u32 offset) {
        const u32 index = offset >> 3;
        if (!(offset & 7) && index < m_decoded.size() && m_decoded[index]) return m_code[index];
        return AtSlow(offset);
    }
    // Numero de registros que usa como mucho (para no limpiar los 256 cada vez)
    static constexpr u32 MAX_CODE = 0x40000;   // 256 KB: un shader nunca es tan grande

private:
    u64 m_address;
    ReadFn m_read;
    ShaderHeader m_header;
    std::vector<ShaderInstr> m_code;     // indexado por offset / 8
    std::vector<u8> m_decoded;
    const ShaderInstr& AtSlow(u32 offset);
};

// ----------------------------------------------------------------------------
// Interprete: ejecuta un hilo
// ----------------------------------------------------------------------------
class ShaderInterpreter {
public:
    struct Result {
        bool ok = true;        // false = instruccion desconocida o bucle infinito
        bool killed = false;   // KIL (pixel descartado)
        std::string error;
    };
    // Ejecuta desde el principio. Los registros quedan en Reg() para leer las salidas
    // del shader de pixeles (colores en R0.., profundidad...).
    Result Run(ShaderProgram& program, ShaderEnv& env);

    u32 Reg(u32 i) const { return i < 255 ? m_r[i] : 0; }
    float RegF(u32 i) const;
    void SetReg(u32 i, u32 v) { if (i < 255) { m_r[i] = v; if (i >= m_dirty) m_dirty = i + 1; } }

    // Limite de instrucciones por hilo (evita colgarse con un bucle infinito)
    u64 max_steps = 1'000'000;
    u64 steps = 0;   // instrucciones ejecutadas en la ultima llamada a Run

    struct StackEntry {
        ShOp kind;    // Ssy, Pbk, Pcnt o Cal
        u32 target;
    };

private:
    std::vector<StackEntry> m_stack;
    std::vector<u8> m_local;
    std::array<u32, 256> m_r{};
    u32 m_dirty = 0;   // registros [0, m_dirty) pueden no ser 0 (Run solo limpia esos)
    std::array<bool, 8> m_p{};
    bool m_ccZero = false, m_ccSign = false, m_ccCarry = false, m_ccOverflow = false;
};

} // namespace NeXo2::GPU
