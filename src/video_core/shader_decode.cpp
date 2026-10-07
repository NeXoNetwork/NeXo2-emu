// Decodificacion de instrucciones Maxwell: de 64 bits a ShaderInstr.
// Ver shader.hpp y docs/07-nexo-internals/gpu-shaders.md.
#include "shader.hpp"

namespace NeXo2::GPU {

namespace {

enum class ImmKind : u8 { None, Float, Int, U16 };

// Una fila de la tabla: si (palabra_alta & mask) == match, es esta instruccion.
// Las instrucciones de Maxwell tienen el codigo de operacion en los bits altos y de
// longitud variable, por eso cada fila lleva su propia mascara.
struct Entry {
    u32 mask, match;
    ShOp op;
    ShForm form;
    ImmKind imm;
};

// Familias de 2 operandos: registro (0x5Cxx), constbuf (0x4Cxx) e inmediato (0x38xx).
// El inmediato usa el bit 56 como signo, por eso su mascara no mira ese bit.
#define FAM2(op, reg, cb, im, kind) \
    {0xFFF80000u, reg, ShOp::op, ShForm::Reg, ImmKind::None}, \
    {0xFFF80000u, cb,  ShOp::op, ShForm::Cbuf, ImmKind::None}, \
    {0xFEF80000u, im,  ShOp::op, ShForm::Imm, ImmKind::kind}
// Comparaciones (campos hasta el bit 51): mascara de 12 bits
#define FAMCMP(op, reg, cb, im, kind) \
    {0xFFF00000u, reg, ShOp::op, ShForm::Reg, ImmKind::None}, \
    {0xFFF00000u, cb,  ShOp::op, ShForm::Cbuf, ImmKind::None}, \
    {0xFEF00000u, im,  ShOp::op, ShForm::Imm, ImmKind::kind}

const Entry kTable[] = {
    // --- Control de flujo ---
    {0xFFF00000u, 0xE3000000u, ShOp::Exit, ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE2400000u, ShOp::Bra,  ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE2100000u, ShOp::Jmp,  ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE2900000u, ShOp::Ssy,  ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xF0F80000u, ShOp::Sync, ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE2A00000u, ShOp::Pbk,  ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE3400000u, ShOp::Brk,  ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE2B00000u, ShOp::Pcnt, ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE3500000u, ShOp::Cont, ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE2600000u, ShOp::Cal,  ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE3200000u, ShOp::Ret,  ShForm::None, ImmKind::None},
    {0xFFF00000u, 0xE3300000u, ShOp::Kil,  ShForm::None, ImmKind::None},
    {0xFFF80000u, 0x50B00000u, ShOp::Nop,  ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xF0F00000u, ShOp::Nop,  ShForm::None, ImmKind::None},   // DEPBAR
    {0xFFF80000u, 0xF0A80000u, ShOp::Nop,  ShForm::None, ImmKind::None},   // BAR (un hilo: nada)
    {0xFFF80000u, 0xEF980000u, ShOp::Nop,  ShForm::None, ImmKind::None},   // MEMBAR

    // --- Movimientos y conversiones ---
    FAM2(Mov, 0x5C980000u, 0x4C980000u, 0x38980000u, Int),
    {0xFFF00000u, 0x01000000u, ShOp::Mov32i, ShForm::Imm32, ImmKind::None},
    {0xFFF80000u, 0xF0C80000u, ShOp::S2R, ShForm::None, ImmKind::None},
    FAM2(Sel, 0x5CA00000u, 0x4CA00000u, 0x38A00000u, Int),
    FAM2(F2F, 0x5CA80000u, 0x4CA80000u, 0x38A80000u, Float),
    FAM2(F2I, 0x5CB00000u, 0x4CB00000u, 0x38B00000u, Float),
    FAM2(I2F, 0x5CB80000u, 0x4CB80000u, 0x38B80000u, Int),
    FAM2(I2I, 0x5CE00000u, 0x4CE00000u, 0x38E00000u, Int),

    // --- Coma flotante ---
    FAM2(Fadd, 0x5C580000u, 0x4C580000u, 0x38580000u, Float),
    {0xFC000000u, 0x08000000u, ShOp::Fadd32i, ShForm::Imm32, ImmKind::None},
    FAM2(Fmul, 0x5C680000u, 0x4C680000u, 0x38680000u, Float),
    {0xFF000000u, 0x1E000000u, ShOp::Fmul32i, ShForm::Imm32, ImmKind::None},
    {0xFF800000u, 0x59800000u, ShOp::Ffma, ShForm::Reg, ImmKind::None},
    {0xFF800000u, 0x49800000u, ShOp::Ffma, ShForm::Cbuf, ImmKind::None},
    {0xFE800000u, 0x32800000u, ShOp::Ffma, ShForm::Imm, ImmKind::Float},
    {0xFF800000u, 0x51800000u, ShOp::Ffma, ShForm::CbufC, ImmKind::None},
    {0xFC000000u, 0x0C000000u, ShOp::Ffma32i, ShForm::Imm32, ImmKind::None},
    {0xFFF80000u, 0x50800000u, ShOp::Mufu, ShForm::None, ImmKind::None},
    FAM2(Fmnmx, 0x5C600000u, 0x4C600000u, 0x38600000u, Float),
    FAM2(Rro, 0x5C900000u, 0x4C900000u, 0x38900000u, Float),
    {0xFF000000u, 0x58000000u, ShOp::Fset, ShForm::Reg, ImmKind::None},
    {0xFF000000u, 0x48000000u, ShOp::Fset, ShForm::Cbuf, ImmKind::None},
    {0xFE000000u, 0x30000000u, ShOp::Fset, ShForm::Imm, ImmKind::Float},
    FAMCMP(Fsetp, 0x5BB00000u, 0x4BB00000u, 0x36B00000u, Float),
    FAMCMP(Fcmp, 0x5BA00000u, 0x4BA00000u, 0x36A00000u, Float),
    {0xFFF00000u, 0x53A00000u, ShOp::Fcmp, ShForm::CbufC, ImmKind::None},

    // --- Enteros ---
    FAM2(Lop, 0x5C400000u, 0x4C400000u, 0x38400000u, Int),
    {0xFC000000u, 0x04000000u, ShOp::Lop32i, ShForm::Imm32, ImmKind::None},
    FAM2(Iadd, 0x5C100000u, 0x4C100000u, 0x38100000u, Int),
    {0xFE000000u, 0x1C000000u, ShOp::Iadd32i, ShForm::Imm32, ImmKind::None},
    FAM2(Imul, 0x5C380000u, 0x4C380000u, 0x38380000u, Int),
    {0xFF000000u, 0x1F000000u, ShOp::Imul32i, ShForm::Imm32, ImmKind::None},
    {0xFF800000u, 0x5A000000u, ShOp::Imad, ShForm::Reg, ImmKind::None},
    {0xFF800000u, 0x4A000000u, ShOp::Imad, ShForm::Cbuf, ImmKind::None},
    {0xFE800000u, 0x34000000u, ShOp::Imad, ShForm::Imm, ImmKind::Int},
    {0xFF800000u, 0x52000000u, ShOp::Imad, ShForm::CbufC, ImmKind::None},
    FAM2(Iscadd, 0x5C180000u, 0x4C180000u, 0x38180000u, Int),
    {0xFFC00000u, 0x5B000000u, ShOp::Xmad, ShForm::Reg, ImmKind::None},
    {0xFF800000u, 0x51000000u, ShOp::Xmad, ShForm::CbufC, ImmKind::None},
    {0xFE000000u, 0x4E000000u, ShOp::Xmad, ShForm::Cbuf, ImmKind::None},
    {0xFEC00000u, 0x36000000u, ShOp::Xmad, ShForm::Imm, ImmKind::U16},
    FAM2(Imnmx, 0x5C200000u, 0x4C200000u, 0x38200000u, Int),
    FAMCMP(Iset, 0x5B500000u, 0x4B500000u, 0x36500000u, Int),
    FAMCMP(Isetp, 0x5B600000u, 0x4B600000u, 0x36600000u, Int),
    FAMCMP(Icmp, 0x5B400000u, 0x4B400000u, 0x36400000u, Int),
    {0xFFF00000u, 0x53400000u, ShOp::Icmp, ShForm::CbufC, ImmKind::None},
    FAM2(Shl, 0x5C480000u, 0x4C480000u, 0x38480000u, Int),
    FAM2(Shr, 0x5C280000u, 0x4C280000u, 0x38280000u, Int),
    FAM2(Popc, 0x5C080000u, 0x4C080000u, 0x38080000u, Int),
    FAM2(Bfi, 0x5BF00000u, 0x4BF00000u, 0x36F00000u, Int),
    {0xFFF80000u, 0x53F00000u, ShOp::Bfi, ShForm::CbufC, ImmKind::None},
    FAM2(Bfe, 0x5C000000u, 0x4C000000u, 0x38000000u, Int),
    FAM2(Flo, 0x5C300000u, 0x4C300000u, 0x38300000u, Int),

    // --- Memoria y atributos ---
    {0xFFF80000u, 0xEFD80000u, ShOp::Ald, ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xEFF00000u, ShOp::Ast, ShForm::None, ImmKind::None},
    {0xFF000000u, 0xE0000000u, ShOp::Ipa, ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xEF900000u, ShOp::Ldc, ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xEF400000u, ShOp::Ldl, ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xEF500000u, ShOp::Stl, ShForm::None, ImmKind::None},

    // --- Texturas ---
    {0xFE000000u, 0xD8000000u, ShOp::Texs, ShForm::None, ImmKind::None},
    {0xFE000000u, 0xDA000000u, ShOp::Tlds, ShForm::None, ImmKind::None},
    {0xFE380000u, 0xC0380000u, ShOp::Tex,  ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xDE380000u, ShOp::Txd,  ShForm::None, ImmKind::None},
    {0xFFF80000u, 0xDF480000u, ShOp::Txq,  ShForm::None, ImmKind::None},
};
#undef FAM2
#undef FAMCMP

} // namespace

ShaderInstr DecodeShaderInstr(u64 raw) {
    ShaderInstr in;
    in.raw = raw;
    const u32 hi = u32(raw >> 32);
    const Entry* found = nullptr;
    for (const Entry& e : kTable)
        if ((hi & e.mask) == e.match) { found = &e; break; }
    if (!found) return in;   // Invalid

    in.op = found->op;
    in.form = found->form;
    // SSY, PBK, PCNT y CAL no llevan predicado: esos bits son parte de la direccion destino
    const bool predicated = in.op != ShOp::Ssy && in.op != ShOp::Pbk && in.op != ShOp::Pcnt && in.op != ShOp::Cal;
    in.pred = predicated ? u8(in.Bits(16, 3)) : 7;
    in.pred_neg = predicated && in.Bit(19);
    in.rd = u8(in.Bits(0, 8));
    in.ra = u8(in.Bits(8, 8));
    in.rc = u8(in.Bits(39, 8));
    switch (in.form) {
        case ShForm::Reg:
            in.rb = u8(in.Bits(20, 8));
            break;
        case ShForm::Cbuf:
            in.cb_index = u8(in.Bits(34, 5));
            in.cb_offset = in.Bits(20, 14) * 4;
            break;
        case ShForm::CbufC:
            in.rb = u8(in.Bits(39, 8));   // B es el registro de la posicion 39
            in.rc = 0xFF;
            in.cb_index = u8(in.Bits(34, 5));
            in.cb_offset = in.Bits(20, 14) * 4;
            break;
        case ShForm::Imm: {
            const u32 v = in.Bits(20, 19) | (u32(in.Bit(56)) << 19);   // 20 bits con signo
            switch (found->imm) {
                case ImmKind::Float: in.imm = v << 12; break;               // los 20 bits altos del float
                case ImmKind::Int:   in.imm = (v & 0x80000) ? (v | 0xFFF00000u) : v; break;
                case ImmKind::U16:   in.imm = in.Bits(20, 16); break;
                default: break;
            }
            break;
        }
        case ShForm::Imm32:
            in.imm = in.Bits(20, 32);
            break;
        default:
            break;
    }
    return in;
}

const char* ShOpName(ShOp op) {
    static const char* const names[] = {
        "invalid",
        "exit", "bra", "jmp", "ssy", "sync", "pbk", "brk", "pcnt", "cont", "cal", "ret", "kil", "nop",
        "mov", "mov32i", "s2r", "sel", "f2f", "f2i", "i2f", "i2i",
        "fadd", "fadd32i", "fmul", "fmul32i", "ffma", "ffma32i", "mufu", "fmnmx", "rro", "fset", "fsetp", "fcmp",
        "lop", "lop32i", "iadd", "iadd32i", "imul", "imul32i", "imad", "iscadd", "xmad", "imnmx", "iset", "isetp", "icmp",
        "shl", "shr", "popc", "bfi", "bfe", "flo",
        "ald", "ast", "ipa", "ldc", "ldl", "stl",
        "tex", "texs", "tlds", "txd", "txq",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(ShOp::Count), "faltan nombres");
    return names[size_t(op)];
}

// ----------------------------------------------------------------------------
// Programa
// ----------------------------------------------------------------------------

ShaderProgram::ShaderProgram(u64 address, ReadFn read) : m_address(address), m_read(std::move(read)) {
    m_read(address, m_header.words, sizeof(m_header.words));
}

const ShaderInstr& ShaderProgram::AtSlow(u32 offset) {
    static const ShaderInstr invalid{};
    if (offset >= MAX_CODE || (offset & 7)) return invalid;
    const u32 index = offset / 8;
    if (index >= m_code.size()) {
        // Crecer de 4 KB en 4 KB
        const size_t want = (size_t(index) / 512 + 1) * 512;
        m_code.resize(want);
        m_decoded.resize(want, 0);
    }
    if (!m_decoded[index]) {
        // Leer el grupo de 32 bytes entero (planificacion + 3 instrucciones)
        const u32 group = index & ~3u;
        u64 words[4];
        m_read(m_address + 0x50 + u64(group) * 8, words, sizeof(words));
        for (u32 i = 1; i < 4; ++i) {
            m_code[group + i] = DecodeShaderInstr(words[i]);
            m_decoded[group + i] = 1;
        }
        m_code[group] = ShaderInstr{};    // la palabra de planificacion no se ejecuta
        m_code[group].op = ShOp::Nop;
        m_decoded[group] = 1;
    }
    return m_code[index];
}

} // namespace NeXo2::GPU
