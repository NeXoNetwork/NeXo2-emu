// Traductor de shaders Maxwell -> SPIR-V. Ver shader_spirv.hpp.
// Cada instruccion se traduce con las mismas cuentas que el interprete (shader_exec.cpp),
// incluidos los casos raros (NaN, desnormales, saturaciones), para que los resultados
// de la GPU y del dibujo por software coincidan.
#include "shader_spirv.hpp"
#include "spirv_builder.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace NeXo2::GPU {

namespace {
using namespace Spv;

// Tipos de entrada de la pila de control (SSY/PBK/PCNT/CAL)
constexpr u32 STACK_SSY = 1, STACK_PBK = 2, STACK_PCNT = 3, STACK_CAL = 4;
constexpr u32 STACK_SIZE = 32;
constexpr u32 LOCAL_WORDS = 1024;   // memoria local (LDL/STL): 4 KB
constexpr u32 MAX_INSTRUCTIONS = 1u << 15;

class Translator {
public:
    Translator(ShaderProgram& prog, const SpirvOptions& opt) : m_prog(prog), m_opt(opt) {}

    SpirvResult Run() {
        SpirvResult res;
        if (!Discover()) { res.error = m_error; return res; }
        if (Frag() && !ScanFragment()) { res.error = m_error; return res; }
        if (Vert() && !ScanVertex()) { res.error = m_error; return res; }
        Setup();
        EmitMain();
        if (!m_error.empty()) { res.error = m_error; return res; }
        res.ok = true;
        res.code = b.Assemble();
        res.cbuf_mask = m_cbufMask;
        res.instructions = u32(m_instrs.size());
        res.code_end = 0x50 + (m_instrs.empty() ? 0 : m_instrs.rbegin()->first + 8);
        res.varyings = m_varyings;
        res.textures = m_textures;
        res.writes_depth = m_fragDepth != 0;
        for (u32 i = 0; i < 18; ++i)
            if (m_cbufMask & (1u << i)) res.cbuf_bytes[i] = (m_cbufDynamic & (1u << i)) ? 0x10000u : std::min(m_cbufEnd[i], 0x10000u);
        res.vs_attr_mask = m_vsAttrMask;
        res.vs_vertex_id = m_vsVertexId;
        return res;
    }

private:
    // =====================================================================
    //  1. Descubrir el codigo: que instrucciones hay y donde empieza cada bloque
    // =====================================================================
    static u32 NextPc(u32 p) { p += 8; if ((p & 31) == 0) p += 8; return p; }   // salta la planificacion
    static u32 RelTarget(const ShaderInstr& in, u32 pc) { return u32(s32(pc + 8) + (s32(in.Bits(20, 24) << 8) >> 8)); }

    bool Fail(const std::string& what) { if (m_error.empty()) m_error = what; return false; }

    bool Discover() {
        std::vector<u32> work{8};
        m_leaders.insert(8);
        auto add_leader = [&](u32 t) { if (m_leaders.insert(t).second || !m_instrs.count(t)) work.push_back(t); };
        while (!work.empty()) {
            u32 pc = work.back();
            work.pop_back();
            while (true) {
                if (m_instrs.count(pc)) break;
                if (m_instrs.size() >= MAX_INSTRUCTIONS || pc >= ShaderProgram::MAX_CODE) return Fail("shader demasiado grande");
                const ShaderInstr in = m_prog.At(pc);
                if (in.op == ShOp::Invalid) {
                    char buf[80];
                    std::snprintf(buf, sizeof(buf), "instruccion desconocida en 0x%X (0x%016llX)", pc, (unsigned long long)in.raw);
                    return Fail(buf);
                }
                m_instrs[pc] = in;
                const u32 npc = NextPc(pc);
                const bool always = in.pred == 7 && !in.pred_neg;
                bool ends = false;
                switch (in.op) {
                    case ShOp::Exit:   // el bloque termina aqui; si puede no salir, sigue en npc
                        ends = true;
                        if (!always || in.Bits(0, 5) != 0xF) add_leader(npc);
                        break;
                    case ShOp::Bra:
                        add_leader(RelTarget(in, pc));
                        ends = true;
                        if (!always || in.Bits(0, 5) != 0xF) add_leader(npc);
                        break;
                    case ShOp::Jmp:
                        add_leader(in.Bits(20, 32));
                        ends = true;
                        if (!always) add_leader(npc);
                        break;
                    case ShOp::Ssy: case ShOp::Pbk: case ShOp::Pcnt:
                        add_leader(RelTarget(in, pc));
                        break;
                    case ShOp::Cal:
                        add_leader(RelTarget(in, pc));
                        add_leader(npc);   // aqui se vuelve con RET
                        ends = true;
                        break;
                    case ShOp::Sync: case ShOp::Brk: case ShOp::Cont: case ShOp::Ret:
                        ends = true;       // el destino sale de la pila (ya es un bloque)
                        if (!always) add_leader(npc);
                        break;
                    case ShOp::Kil:
                        ends = true;
                        if (!always || in.Bits(0, 5) != 0xF) add_leader(npc);
                        break;
                    default:
                        break;
                }
                if (ends) break;
                pc = npc;
            }
        }
        return true;
    }

    bool Frag() const { return m_opt.stage == SpirvOptions::Stage::Fragment; }
    bool Vert() const { return m_opt.stage == SpirvOptions::Stage::Vertex; }

    // Shader de vertices: que atributos lee y escribe (solo con direcciones fijas)
    bool ScanVertex() {
        for (const auto& [pc, in] : m_instrs) {
            switch (in.op) {
                case ShOp::Ald: case ShOp::Ast: {
                    if (in.ra != 0xFF) return Fail("ALD/AST con direccion en un registro (vertices en la GPU)");
                    if (in.Bit(31) || (in.op == ShOp::Ald && in.Bit(32))) return Fail("ALD de parche o de salida en el shader de vertices");
                    if (in.op == ShOp::Ast) break;
                    const u32 count = in.Bits(47, 2) + 1, base = in.Bits(20, 10);
                    for (u32 i = 0; i < count; ++i) {
                        const u32 a = base + 4 * i;
                        if (a >= 0x80 && a < 0x280) m_vsAttrMask |= 1u << ((a - 0x80) / 16);
                        else if (a == 0x2FC) m_vsVertexId = true;
                    }
                    break;
                }
                case ShOp::Ipa:
                    return Fail("IPA en el shader de vertices");
                default:
                    break;
            }
        }
        return true;
    }

    // Forma de la textura que pide una instruccion: dimensiones y si es un array
    bool TexShape(const ShaderInstr& in, u8& dim, bool& array) {
        array = false;
        if (in.op == ShOp::Texs) {
            //                       0  1  2  3  4  5  6  7  8  9 10 11 12 13
            static const s8 dims[16] = {0, 1, 1, 1, -1, -1, -1, 1, 1, -1, 2, 2, 3, 3, -2, -2};
            const u32 t = in.Bits(53, 4);
            if (dims[t] == -1) return Fail("textura con comparacion de profundidad (TEXS) todavia no");
            if (dims[t] < 0) return Fail("variante de TEXS no soportada");
            dim = u8(dims[t]);
            array = t == 7 || t == 8;
            return true;
        }
        if (in.op == ShOp::Tlds) {
            static const s8 dims[16] = {0, 0, 1, -1, 1, 1, 1, 2, 1, -1, -1, -1, 1, -1, -1, -1};
            const u32 t = in.Bits(53, 4);
            if (dims[t] < 0) return Fail("variante de TLDS no soportada");
            dim = u8(dims[t]);
            array = t == 8;
            return true;
        }
        // TEX / TXD
        dim = u8(in.Bits(29, 2));
        array = in.Bit(28);
        if (in.op == ShOp::Tex && in.Bit(50)) return Fail("textura con comparacion de profundidad (TEX) todavia no");
        if (dim == 3 && array) return Fail("arrays de cubos todavia no");
        if (dim == 2 && array) return Fail("textura 3D con capas: no existe");
        return true;
    }
    s32 TextureIndex(u32 slot) const {
        for (size_t i = 0; i < m_textures.size(); ++i) if (m_textures[i].slot == slot) return s32(i);
        return -1;
    }

    // Shader de pixeles: que entradas (IPA) y texturas usa
    bool ScanFragment() {
        std::set<u32> query_only;
        for (const auto& [pc, in] : m_instrs) {
            switch (in.op) {
                case ShOp::Ipa: {
                    if (in.Bit(38) && in.ra != 0xFF) return Fail("IPA con direccion en un registro todavia no");
                    const u32 addr = in.Bits(28, 10), mode = in.Bits(54, 2);
                    if (addr >= 0x70 && addr < 0x80) m_needFragCoord = true;
                    else if (addr == 0x3FC) m_needFront = true;
                    else if (addr < 0x400) {
                        SpirvVarying v;
                        v.slot = u8(addr / 16);
                        v.kind = mode == 2 ? SpirvVarying::Flat : mode == 0 ? SpirvVarying::Linear : SpirvVarying::Perspective;
                        if (VaryingIndex(v.slot, v.kind) < 0) m_varyings.push_back(v);
                        if (v.kind == SpirvVarying::Perspective) m_needFragCoord = true;
                    }
                    break;
                }
                case ShOp::Ald:
                    m_needFragCoord = m_needFront = true;
                    break;
                case ShOp::Texs: case ShOp::Tlds: case ShOp::Tex: case ShOp::Txd: {
                    SpirvTexture t;
                    t.slot = in.Bits(36, 13);
                    if (!TexShape(in, t.dim, t.array)) return false;
                    const s32 k = TextureIndex(t.slot);
                    if (k < 0) m_textures.push_back(t);
                    else if (m_textures[k].dim != t.dim || m_textures[k].array != t.array)
                        return Fail("la misma textura se usa con formas distintas");
                    break;
                }
                case ShOp::Txq:
                    query_only.insert(in.Bits(36, 13));
                    break;
                default:
                    break;
            }
        }
        for (u32 slot : query_only)
            if (TextureIndex(slot) < 0) { SpirvTexture t; t.slot = slot; m_textures.push_back(t); }   // solo TXQ: 2D
        return true;
    }
    s32 VaryingIndex(u32 slot, SpirvVarying::Kind kind) const {
        for (size_t i = 0; i < m_varyings.size(); ++i)
            if (m_varyings[i].slot == slot && m_varyings[i].kind == kind) return s32(i);
        return -1;
    }

    // =====================================================================
    //  2. Tipos, constantes y variables globales
    // =====================================================================
    void Setup() {
        b.Capability(1);                       // Shader
        if (m_opt.int64) b.Capability(11);     // Int64
        m_glsl = b.ImportGlsl();
        Builder::Emit(b.memory_model, OpMemoryModel, {0 /* Logical */, 1 /* GLSL450 */});
        tVoid = b.TypeVoid();
        tBool = b.TypeBool();
        tU32 = b.TypeInt(32, false);
        tS32 = b.TypeInt(32, true);
        tF32 = b.TypeFloat(32);
        tV2F = b.TypeVector(tF32, 2);
        if (m_opt.int64) { tU64 = b.TypeInt(64, false); tS64 = b.TypeInt(64, true); }
        tPtrFnU32 = b.TypePointer(Function, tU32);
        tPtrFnBool = b.TypePointer(Function, tBool);

        if (m_opt.stage == SpirvOptions::Stage::ComputeTest) {
            m_attrIn = StorageArray(SpirvBinding::ATTR_IN, "attr_in", true);
            m_attrOut = StorageArray(SpirvBinding::ATTR_OUT, "attr_out", false);
        }
        if (Frag()) SetupFragment();
        if (Vert()) SetupVertex();
    }

    // Entradas y salidas del shader de vertices
    void SetupVertex() {
        b.Capability(32);   // ClipDistance
        tV4F = b.TypeVector(tF32, 4);
        tV4U = b.TypeVector(tU32, 4);
        auto io = [&](u32 type, u32 storage, const std::string& name) {
            const u32 var = b.GlobalVariable(b.TypePointer(storage, type), storage);
            b.Name(var, name);
            m_interface.push_back(var);
            return var;
        };
        m_vertexIndex = io(tS32, Input, "vertex_index");
        b.Decorate(m_vertexIndex, DecBuiltIn, {42 /* VertexIndex */});
        m_posOut = io(tV4F, Output, "position");
        b.Decorate(m_posOut, DecBuiltIn, {BuiltInPosition});
        m_clipOut = io(b.TypeArray(tF32, CU(2)), Output, "clip_distance");
        b.Decorate(m_clipOut, DecBuiltIn, {3 /* ClipDistance */});
        for (size_t i = 0; i < m_opt.vs_outputs.size(); ++i) {
            const bool flat = m_opt.vs_outputs[i].kind == SpirvVarying::Flat;
            const u32 var = io(flat ? tV4U : tV4F, Output, "out" + std::to_string(i));
            b.Decorate(var, DecLocation, {u32(i)});
            if (flat) b.Decorate(var, DecFlat);
            else if (m_opt.vs_outputs[i].kind == SpirvVarying::Linear) b.Decorate(var, DecNoPerspective);
            m_varVars.push_back(var);
        }
        m_vsAttr = StorageArray(SpirvBinding::VS_ATTR, "vs_attr", true);
        m_vsInfo = StorageArray(SpirvBinding::VS_INFO, "vs_info", true);
    }

    // Entradas, salidas y texturas del shader de pixeles
    void SetupFragment() {
        tV4F = b.TypeVector(tF32, 4);
        tV4U = b.TypeVector(tU32, 4);
        tV4S = b.TypeVector(tS32, 4);
        auto io = [&](u32 type, u32 storage, const std::string& name) {
            const u32 var = b.GlobalVariable(b.TypePointer(storage, type), storage);
            b.Name(var, name);
            m_interface.push_back(var);
            return var;
        };
        for (size_t i = 0; i < m_varyings.size(); ++i) {
            const bool flat = m_varyings[i].kind == SpirvVarying::Flat;
            const u32 var = io(flat ? tV4U : tV4F, Input, "in" + std::to_string(i));
            b.Decorate(var, DecLocation, {u32(i)});
            if (flat) b.Decorate(var, DecFlat);
            else if (m_varyings[i].kind == SpirvVarying::Linear) b.Decorate(var, DecNoPerspective);
            m_varVars.push_back(var);
        }
        if (m_needFragCoord) { m_fragCoord = io(tV4F, Input, "frag_coord"); b.Decorate(m_fragCoord, DecBuiltIn, {BuiltInFragCoord}); }
        if (m_needFront) { m_front = io(tBool, Input, "front_facing"); b.Decorate(m_front, DecBuiltIn, {BuiltInFrontFacing}); }
        for (u32 k = 0; k < std::min<u32>(m_opt.color_outputs, 8); ++k) {
            const u32 type = m_opt.color_int[k] == 1 ? tV4S : m_opt.color_int[k] == 2 ? tV4U : tV4F;
            const u32 var = io(type, Output, "color" + std::to_string(k));
            b.Decorate(var, DecLocation, {k});
            m_colorVars.push_back(var);
        }
        if (m_prog.Header().OmapDepth()) {
            m_fragDepth = io(tF32, Output, "frag_depth");
            b.Decorate(m_fragDepth, DecBuiltIn, {BuiltInFragDepth});
        }
        bool query = false;
        for (const auto& [pc, in] : m_instrs) query = query || in.op == ShOp::Txq;
        if (query) b.Capability(50);   // ImageQuery
        for (size_t k = 0; k < m_textures.size(); ++k) {
            const SpirvTexture& t = m_textures[k];
            if (t.dim == 0) b.Capability(43);   // Sampled1D
            const u32 img = b.TypeImage(tF32, t.dim, 0, t.array ? 1 : 0, 0, 1, 0);
            const u32 sim = b.TypeSampledImage(img);
            const u32 var = b.GlobalVariable(b.TypePointer(UniformConstant, sim), UniformConstant);
            b.Decorate(var, DecDescriptorSet, {0});
            b.Decorate(var, DecBinding, {SpirvBinding::TEXTURE0 + u32(k)});
            b.Name(var, "tex" + std::to_string(t.slot));
            m_texImg.push_back(img);
            m_texSim.push_back(sim);
            m_texVar.push_back(var);
        }
    }

    // Storage buffer "struct { uint data[]; }" en el binding 'binding'
    u32 StorageArray(u32 binding, const char* name, bool readonly) {
        const u32 rt = b.TypeRuntimeArray(tU32);
        if (!m_arrayStrideDone) { b.Decorate(rt, DecArrayStride, {4}); m_arrayStrideDone = true; }
        const u32 st = b.TypeStruct({rt});
        b.Decorate(st, DecBlock);
        b.MemberDecorate(st, 0, DecOffset, {0});
        if (readonly) b.MemberDecorate(st, 0, DecNonWritable);
        const u32 var = b.GlobalVariable(b.TypePointer(StorageBuffer, st), StorageBuffer);
        b.Decorate(var, DecDescriptorSet, {0});
        b.Decorate(var, DecBinding, {binding});
        b.Name(var, name);
        return var;   // (en SPIR-V 1.3 la lista del punto de entrada solo lleva Input/Output)
    }

    u32 Cbuf(u32 index) {
        auto it = m_cbufs.find(index);
        if (it != m_cbufs.end()) return it->second;
        const u32 var = StorageArray(index + (Vert() ? SpirvBinding::VS_CBUF0 : 0), ("c" + std::to_string(index)).c_str(), true);
        m_cbufs[index] = var;
        m_cbufMask |= 1u << index;
        return var;
    }

    // =====================================================================
    //  Ayudas para escribir el cuerpo de la funcion
    // =====================================================================
    void E(u32 op, std::initializer_list<u32> ops) { Builder::Emit(m_code, op, ops); }
    void E(u32 op, const std::vector<u32>& ops) { Builder::Emit(m_code, op, ops); }
    u32 Val(u32 op, u32 type, std::initializer_list<u32> args) {
        const u32 id = b.Id();
        std::vector<u32> ops{type, id};
        ops.insert(ops.end(), args.begin(), args.end());
        E(op, ops);
        return id;
    }
    u32 Ext(u32 type, u32 inst, std::initializer_list<u32> args) {
        const u32 id = b.Id();
        std::vector<u32> ops{type, id, m_glsl, inst};
        ops.insert(ops.end(), args.begin(), args.end());
        E(OpExtInst, ops);
        return id;
    }
    void Label(u32 id) { E(OpLabel, {id}); }
    u32 Load(u32 type, u32 ptr) { return Val(OpLoad, type, {ptr}); }
    void Store(u32 ptr, u32 v) { E(OpStore, {ptr, v}); }

    // Variable de la funcion (se declaran todas al principio, en el bloque de entrada)
    u32 FnVar(u32 ptr_type, u32 init) {
        const u32 id = b.Id();
        Builder::Emit(m_vars, OpVariable, {ptr_type, id, Function});
        if (init) m_inits.push_back({id, init});
        return id;
    }

    // Constantes
    u32 CU(u32 v) { return b.ConstU32(v); }
    u32 CF(float f) { return b.ConstF32(f); }
    u32 CB(bool v) { return b.ConstBool(v); }

    // Conversiones de bits
    u32 AsF(u32 u) { return Val(OpBitcast, tF32, {u}); }
    u32 AsU(u32 f) { return Val(OpBitcast, tU32, {f}); }
    u32 AsS(u32 u) { return Val(OpBitcast, tS32, {u}); }
    u32 SToU(u32 s) { return Val(OpBitcast, tU32, {s}); }

    // Operaciones de uint
    u32 And(u32 a, u32 c) { return Val(OpBitwiseAnd, tU32, {a, c}); }
    u32 Or(u32 a, u32 c) { return Val(OpBitwiseOr, tU32, {a, c}); }
    u32 Xor(u32 a, u32 c) { return Val(OpBitwiseXor, tU32, {a, c}); }
    u32 Not(u32 a) { return Val(OpNot, tU32, {a}); }
    u32 Add(u32 a, u32 c) { return Val(OpIAdd, tU32, {a, c}); }
    u32 Sub(u32 a, u32 c) { return Val(OpISub, tU32, {a, c}); }
    u32 Mul(u32 a, u32 c) { return Val(OpIMul, tU32, {a, c}); }
    u32 Shl(u32 a, u32 s) { return Val(OpShiftLeftLogical, tU32, {a, s}); }
    u32 Shr(u32 a, u32 s) { return Val(OpShiftRightLogical, tU32, {a, s}); }
    u32 Sar(u32 a, u32 s) { return Val(OpShiftRightArithmetic, tU32, {a, s}); }
    u32 Neg(u32 a) { return Sub(CU(0), a); }
    u32 Eq(u32 a, u32 c) { return Val(OpIEqual, tBool, {a, c}); }
    u32 Ne(u32 a, u32 c) { return Val(OpINotEqual, tBool, {a, c}); }
    u32 ULt(u32 a, u32 c) { return Val(OpULessThan, tBool, {a, c}); }
    u32 UGe(u32 a, u32 c) { return Val(OpUGreaterThanEqual, tBool, {a, c}); }
    u32 SLt(u32 a, u32 c) { return Val(OpSLessThan, tBool, {a, c}); }
    u32 Sel(u32 c, u32 a, u32 d, u32 type = 0) { return Val(OpSelect, type ? type : tU32, {c, a, d}); }
    u32 UMin(u32 a, u32 c) { return Sel(ULt(a, c), a, c); }
    u32 LAnd(u32 a, u32 c) { return Val(OpLogicalAnd, tBool, {a, c}); }
    u32 LOr(u32 a, u32 c) { return Val(OpLogicalOr, tBool, {a, c}); }
    u32 LNot(u32 a) { return Val(OpLogicalNot, tBool, {a}); }
    u32 LNe(u32 a, u32 c) { return Val(OpLogicalNotEqual, tBool, {a, c}); }
    u32 Bit(u32 v, u32 bit) { return Ne(And(v, CU(1u << bit)), CU(0)); }
    // Desplazamientos que en SPIR-V no estan definidos para >= 32: aqui dan 0 (como en C++ con la comprobacion)
    u32 ShlSafe(u32 a, u32 s) { return Sel(UGe(s, CU(32)), CU(0), Shl(a, UMin(s, CU(31)))); }
    u32 ShrSafe(u32 a, u32 s) { return Sel(UGe(s, CU(32)), CU(0), Shr(a, UMin(s, CU(31)))); }
    // Mascara de n bits (n de 0 a 32)
    u32 MaskBits(u32 n) { return Sel(UGe(n, CU(32)), CU(0xFFFFFFFFu), Sub(ShlSafe(CU(1), n), CU(1))); }

    // Floats
    u32 FAdd(u32 a, u32 c) { return Val(OpFAdd, tF32, {a, c}); }
    u32 FMul(u32 a, u32 c) { return Val(OpFMul, tF32, {a, c}); }
    u32 FNeg(u32 a) { return Val(OpFNegate, tF32, {a}); }
    u32 FAbs(u32 a) { return Ext(tF32, GlslFAbs, {a}); }
    u32 IsNan(u32 a) { return Val(OpIsNan, tBool, {a}); }
    u32 AbsNeg(u32 f, bool abs, bool neg) { if (abs) f = FAbs(f); return neg ? FNeg(f) : f; }
    // "Flush to zero" sobre los bits: exponente 0 -> solo el signo
    u32 FtzBits(u32 u) { return Sel(Eq(And(u, CU(0x7F800000u)), CU(0)), And(u, CU(0x80000000u)), u); }
    u32 Ftz(u32 f) { return AsF(FtzBits(AsU(f))); }
    u32 Sat(u32 f) { return Ext(tF32, GlslNClamp, {f, CF(0.0f), CF(1.0f)}); }

    // ---- Registros, predicados y banderas ----
    u32 RegVar(u32 i) {
        auto it = m_regs.find(i);
        if (it != m_regs.end()) return it->second;
        const u32 v = FnVar(tPtrFnU32, CU(0));
        b.Name(v, "r" + std::to_string(i));
        m_regs[i] = v;
        return v;
    }
    u32 R(u32 i) { return i >= 255 ? CU(0) : Load(tU32, RegVar(i)); }
    void W(u32 i, u32 v) { if (i < 255) Store(RegVar(i), v); }
    u32 PredVar(u32 i) {
        auto it = m_preds.find(i);
        if (it != m_preds.end()) return it->second;
        const u32 v = FnVar(tPtrFnBool, CB(false));
        b.Name(v, "p" + std::to_string(i));
        m_preds[i] = v;
        return v;
    }
    u32 P(u32 i) { return (i & 7) == 7 ? CB(true) : Load(tBool, PredVar(i & 7)); }
    void WP(u32 i, u32 v) { if ((i & 7) != 7) Store(PredVar(i & 7), v); }
    u32 CcVar(int which) {   // 0 zero, 1 sign, 2 carry, 3 overflow
        if (!m_cc[which]) m_cc[which] = FnVar(tPtrFnBool, CB(false));
        return m_cc[which];
    }
    void SetCcInt(u32 v) {
        Store(CcVar(0), Eq(v, CU(0)));
        Store(CcVar(1), SLt(AsS(v), Val(OpBitcast, tS32, {CU(0)})));
    }

    // c[index][offset] con offset fijo
    u32 ConstStatic(u32 index, u32 offset) {
        if (index < 18) m_cbufEnd[index] = std::max(m_cbufEnd[index], (offset & ~3u) + 4);
        if (Frag() || Vert()) return ConstDynamic(index, CU(offset));   // fuera del constbuf = 0 (como el interprete)
        const u32 var = Cbuf(index);
        const u32 ptr = Val(OpAccessChain, b.TypePointer(StorageBuffer, tU32), {var, CU(0), CU(offset / 4)});
        return Load(tU32, ptr);
    }
    // c[index][offset] con offset calculado: fuera del buffer = 0
    u32 ConstDynamic(u32 index, u32 offset) {
        const u32 var = Cbuf(index);
        const u32 word = Shr(offset, CU(2));
        const u32 len = Val(OpArrayLength, tU32, {var, 0});
        const u32 inside = ULt(word, len);
        const u32 safe = Sel(inside, word, CU(0));
        const u32 ptr = Val(OpAccessChain, b.TypePointer(StorageBuffer, tU32), {var, CU(0), safe});
        return Sel(inside, Load(tU32, ptr), CU(0));
    }

    // Operandos habituales
    u32 A(const ShaderInstr& in) { return R(in.ra); }
    u32 B(const ShaderInstr& in) {
        switch (in.form) {
            case ShForm::Reg: case ShForm::CbufC: return R(in.rb);
            case ShForm::Cbuf: return ConstStatic(in.cb_index, in.cb_offset);
            case ShForm::Imm: case ShForm::Imm32: return CU(in.imm);
            default: return CU(0);
        }
    }
    u32 C(const ShaderInstr& in) {
        if (in.form == ShForm::CbufC) return ConstStatic(in.cb_index, in.cb_offset);
        return R(in.rc);
    }

    // if (cond) { body() }
    void If(u32 cond, const std::function<void()>& body) {
        const u32 then = b.Id(), after = b.Id();
        E(OpSelectionMerge, {after, 0});
        E(OpBranchConditional, {cond, then, after});
        Label(then);
        body();
        E(OpBranch, {after});
        Label(after);
    }

    // =====================================================================
    //  3. Funcion principal: bucle { switch (pc) { bloques } }
    // =====================================================================
    void EmitMain() {
        const u32 fn = b.Id(), entry = b.Id(), header = b.Id(), body = b.Id(), swmerge = b.Id(),
                  cont = b.Id(), merge = b.Id();
        m_pcVar = FnVar(tPtrFnU32, CU(8));
        m_running = FnVar(tPtrFnBool, CB(true));
        b.Name(m_pcVar, "pc");

        E(OpBranch, {header});   // (despues de las variables del bloque de entrada)
        Label(header);
        E(OpLoopMerge, {merge, cont, 0});
        E(OpBranch, {body});
        Label(body);
        const u32 pcv = Load(tU32, m_pcVar);
        std::map<u32, u32> labels;
        for (u32 l : m_leaders) labels[l] = b.Id();
        const u32 bad_pc = b.Id();
        {
            std::vector<u32> ops{pcv, bad_pc};
            for (const auto& [pc, lab] : labels) { ops.push_back(pc); ops.push_back(lab); }
            E(OpSelectionMerge, {swmerge, 0});
            E(OpSwitch, ops);
        }
        for (const auto& [leader, lab] : labels) {
            Label(lab);
            EmitBlock(leader);
            E(OpBranch, {swmerge});
        }
        Label(bad_pc);                 // pc desconocido: parar en vez de dar vueltas para siempre
        Store(m_running, CB(false));
        E(OpBranch, {swmerge});
        Label(swmerge);
        E(OpBranch, {cont});
        Label(cont);
        const u32 run = Load(tBool, m_running);
        E(OpBranchConditional, {run, header, merge});
        Label(merge);
        if (Frag()) EmitFragmentOutputs();
        if (Vert()) EmitVertexOutputs();
        E(OpReturn, {});

        // Cabecera de la funcion: tipo, etiqueta de entrada, variables y sus valores iniciales
        std::vector<u32> head;
        Builder::Emit(head, OpFunction, {tVoid, fn, 0, b.TypeFunction(tVoid)});
        Builder::Emit(head, OpLabel, {entry});
        head.insert(head.end(), m_vars.begin(), m_vars.end());
        for (const auto& [var, init] : m_inits) Builder::Emit(head, OpStore, {var, init});
        if (m_vertexBaseInit) {   // primera palabra del vertice en VS_ATTR
            const u32 vi = b.Id(), vu = b.Id(), base = b.Id();
            Builder::Emit(head, OpLoad, {tS32, vi, m_vertexIndex});
            Builder::Emit(head, OpBitcast, {tU32, vu, vi});
            Builder::Emit(head, OpIMul, {tU32, base, vu, CU(VsRecordWords(m_vsAttrMask, m_vsVertexId))});
            Builder::Emit(head, OpStore, {m_vertexBase, base});
        }
        b.functions.insert(b.functions.end(), head.begin(), head.end());
        b.functions.insert(b.functions.end(), m_code.begin(), m_code.end());
        Builder::Emit(b.functions, OpFunctionEnd, {});

        // Punto de entrada
        std::vector<u32> ep;
        if (m_opt.stage == SpirvOptions::Stage::ComputeTest) {
            ep = {5 /* GLCompute */, fn};
            Builder::PushString(ep, "main");
            Builder::Emit(b.exec_modes, OpExecutionMode, {fn, 17 /* LocalSize */, 1, 1, 1});
        } else if (Frag()) {
            ep = {4 /* Fragment */, fn};
            Builder::PushString(ep, "main");
            ep.insert(ep.end(), m_interface.begin(), m_interface.end());
            Builder::Emit(b.exec_modes, OpExecutionMode, {fn, 7 /* OriginUpperLeft */});
            if (m_fragDepth) Builder::Emit(b.exec_modes, OpExecutionMode, {fn, 12 /* DepthReplacing */});
        } else {
            ep = {0 /* Vertex */, fn};
            Builder::PushString(ep, "main");
            ep.insert(ep.end(), m_interface.begin(), m_interface.end());
        }
        Builder::Emit(b.entry_points, OpEntryPoint, ep);
    }

    // Un bloque: instrucciones desde 'leader' hasta un salto o el siguiente bloque
    void EmitBlock(u32 leader) {
        u32 pc = leader;
        while (m_error.empty()) {
            auto it = m_instrs.find(pc);
            if (it == m_instrs.end()) { Store(m_running, CB(false)); return; }   // fuera del codigo: parar
            const ShaderInstr& in = it->second;
            const u32 npc = NextPc(pc);
            if (in.op == ShOp::Ssy || in.op == ShOp::Pbk || in.op == ShOp::Pcnt) {
                // Solo guardan un destino en la pila: el bloque sigue
                Push(in.op == ShOp::Ssy ? STACK_SSY : in.op == ShOp::Pbk ? STACK_PBK : STACK_PCNT, RelTarget(in, pc));
            } else if (IsControl(in.op)) {
                Store(m_pcVar, CU(npc));   // por defecto, seguir; el salto lo cambia
                EmitControl(in, pc, npc);
                return;
            }
            // Instruccion normal con su predicado de guarda
            else if (in.op != ShOp::Nop) {
                if (in.pred == 7 && in.pred_neg) { /* nunca */ }
                else if (in.pred == 7) EmitInstr(in);
                else {
                    u32 cond = P(in.pred);
                    if (in.pred_neg) cond = LNot(cond);
                    If(cond, [&] { EmitInstr(in); });
                }
            }
            if (m_leaders.count(npc)) { Store(m_pcVar, CU(npc)); return; }
            pc = npc;
        }
    }

    static bool IsControl(ShOp op) {
        switch (op) {
            case ShOp::Exit: case ShOp::Bra: case ShOp::Jmp: case ShOp::Cal: case ShOp::Sync: case ShOp::Brk:
            case ShOp::Cont: case ShOp::Ret: case ShOp::Kil: case ShOp::Ssy: case ShOp::Pbk: case ShOp::Pcnt:
                return true;
            default: return false;
        }
    }

    // ---- Pila de control ----
    void StackVars() {
        if (m_stackKind) return;
        const u32 arr = b.TypeArray(tU32, CU(STACK_SIZE));
        const u32 parr = b.TypePointer(Function, arr);
        m_stackKind = FnVar(parr, 0);
        m_stackTarget = FnVar(parr, 0);
        m_sp = FnVar(tPtrFnU32, CU(0));
        m_found = FnVar(tPtrFnBool, CB(false));
        m_foundTarget = FnVar(tPtrFnU32, CU(0));
    }
    void Push(u32 kind, u32 target) {
        StackVars();
        const u32 sp = Load(tU32, m_sp);
        If(ULt(sp, CU(STACK_SIZE)), [&] {
            Store(Val(OpAccessChain, tPtrFnU32, {m_stackKind, sp}), CU(kind));
            Store(Val(OpAccessChain, tPtrFnU32, {m_stackTarget, sp}), CU(target));
            Store(m_sp, Add(sp, CU(1)));
        });
    }
    // Saca de la pila hasta encontrar 'kind'. Deja m_found y m_foundTarget.
    void Unwind(u32 kind) {
        StackVars();
        Store(m_found, CB(false));
        const u32 header = b.Id(), body = b.Id(), cont = b.Id(), merge = b.Id();
        E(OpBranch, {header});
        Label(header);
        E(OpLoopMerge, {merge, cont, 0});
        E(OpBranch, {body});
        Label(body);
        {
            const u32 sp = Load(tU32, m_sp);
            const u32 empty = Eq(sp, CU(0));
            const u32 brk = b.Id(), go = b.Id(), sel = b.Id();
            E(OpSelectionMerge, {sel, 0});
            E(OpBranchConditional, {empty, brk, go});
            Label(brk);
            E(OpBranch, {merge});
            Label(go);
            const u32 top = Sub(sp, CU(1));
            Store(m_sp, top);
            const u32 k = Load(tU32, Val(OpAccessChain, tPtrFnU32, {m_stackKind, top}));
            const u32 t = Load(tU32, Val(OpAccessChain, tPtrFnU32, {m_stackTarget, top}));
            const u32 hit = b.Id(), miss = b.Id();
            const u32 match = Eq(k, CU(kind));
            E(OpSelectionMerge, {miss, 0});
            E(OpBranchConditional, {match, hit, miss});
            Label(hit);
            Store(m_found, CB(true));
            Store(m_foundTarget, t);
            E(OpBranch, {merge});
            Label(miss);
            E(OpBranch, {sel});
            Label(sel);
            E(OpBranch, {cont});
        }
        Label(cont);
        E(OpBranch, {header});
        Label(merge);
    }

    // ---- Saltos y fin ----
    void EmitControl(const ShaderInstr& in, u32 pc, u32 npc) {
        const bool always = in.pred == 7 && !in.pred_neg;
        if (in.pred == 7 && in.pred_neg) return;   // nunca se ejecuta
        auto guarded = [&](const std::function<void()>& body) {
            if (always) { body(); return; }
            u32 cond = P(in.pred);
            if (in.pred_neg) cond = LNot(cond);
            If(cond, body);
        };
        switch (in.op) {
            case ShOp::Exit:
                if (in.Bits(0, 5) != 0xF && in.Bits(0, 5) != 0) { Fail("condicion de EXIT no soportada"); return; }
                if (in.Bits(0, 5) == 0xF) guarded([&] { Store(m_running, CB(false)); });
                return;
            case ShOp::Kil:
                if (in.Bits(0, 5) != 0xF) return;
                if (m_opt.stage != SpirvOptions::Stage::Fragment) { guarded([&] { Store(m_running, CB(false)); }); return; }
                guarded([&] {
                    // OpKill termina el bloque: lo que sigue (OpBranch del If) queda inalcanzable,
                    // asi que se pone en un bloque propio
                    const u32 dead = b.Id();
                    E(OpKill, {});
                    Label(dead);
                });
                return;
            case ShOp::Bra:
                if (in.Bits(0, 5) != 0xF) { Fail("condicion de BRA no soportada"); return; }
                if (in.Bit(5)) { Fail("BRA a traves de constbuf no soportado"); return; }
                guarded([&] { Store(m_pcVar, CU(RelTarget(in, pc))); });
                return;
            case ShOp::Jmp:
                guarded([&] { Store(m_pcVar, CU(in.Bits(20, 32))); });
                return;
            case ShOp::Ssy: Push(STACK_SSY, RelTarget(in, pc)); return;
            case ShOp::Pbk: Push(STACK_PBK, RelTarget(in, pc)); return;
            case ShOp::Pcnt: Push(STACK_PCNT, RelTarget(in, pc)); return;
            case ShOp::Cal:
                Push(STACK_CAL, npc);
                Store(m_pcVar, CU(RelTarget(in, pc)));
                return;
            case ShOp::Sync: case ShOp::Brk: case ShOp::Cont: case ShOp::Ret: {
                const u32 kind = in.op == ShOp::Sync ? STACK_SSY : in.op == ShOp::Brk ? STACK_PBK
                               : in.op == ShOp::Cont ? STACK_PCNT : STACK_CAL;
                guarded([&] {
                    Unwind(kind);
                    const u32 found = Load(tBool, m_found);
                    Store(m_pcVar, Sel(found, Load(tU32, m_foundTarget), CU(npc)));
                    // Sin entrada en la pila: RET = fin del programa; los demas son un error -> parar
                    Store(m_running, LAnd(Load(tBool, m_running), found));
                });
                return;
            }
            default:
                return;
        }
    }

    // =====================================================================
    //  4. Instrucciones (mismas cuentas que shader_exec.cpp)
    // =====================================================================
    u32 FCompare(u32 cond, u32 a, u32 c) {
        switch (cond & 0xF) {
            case 0:  return CB(false);
            case 1:  return Val(OpFOrdLessThan, tBool, {a, c});
            case 2:  return Val(OpFOrdEqual, tBool, {a, c});
            case 3:  return Val(OpFOrdLessThanEqual, tBool, {a, c});
            case 4:  return Val(OpFOrdGreaterThan, tBool, {a, c});
            case 5:  return Val(OpFOrdNotEqual, tBool, {a, c});
            case 6:  return Val(OpFOrdGreaterThanEqual, tBool, {a, c});
            case 7:  return LNot(LOr(IsNan(a), IsNan(c)));
            case 8:  return LOr(IsNan(a), IsNan(c));
            case 9:  return Val(OpFUnordLessThan, tBool, {a, c});
            case 10: return Val(OpFUnordEqual, tBool, {a, c});
            case 11: return Val(OpFUnordLessThanEqual, tBool, {a, c});
            case 12: return Val(OpFUnordGreaterThan, tBool, {a, c});
            case 13: return Val(OpFUnordNotEqual, tBool, {a, c});
            case 14: return Val(OpFUnordGreaterThanEqual, tBool, {a, c});
            default: return CB(true);
        }
    }
    u32 ICompare(u32 cond, u32 a, u32 c, bool sgn) {
        const u32 lt = sgn ? SLt(AsS(a), AsS(c)) : ULt(a, c);
        const u32 eq = Eq(a, c);
        switch (cond & 7) {
            case 0: return CB(false);
            case 1: return lt;
            case 2: return eq;
            case 3: return LOr(lt, eq);
            case 4: return LNot(LOr(lt, eq));
            case 5: return LNot(eq);
            case 6: return LNot(lt);
            default: return CB(true);
        }
    }
    u32 BoolOp(u32 op, u32 a, u32 c) {
        switch (op & 3) {
            case 0: return LAnd(a, c);
            case 1: return LOr(a, c);
            case 2: return LNe(a, c);
            default: return a;
        }
    }
    u32 RoundMode(u32 f, u32 mode) {
        switch (mode & 3) {
            case 0: return Ext(tF32, GlslRoundEven, {f});
            case 1: return Ext(tF32, GlslFloor, {f});
            case 2: return Ext(tF32, GlslCeil, {f});
            default: return Ext(tF32, GlslTrunc, {f});
        }
    }
    u32 HalfToFloat(u32 h16) { return Val(OpCompositeExtract, tF32, {Ext(tV2F, GlslUnpackHalf2x16, {h16}), 0}); }
    u32 FloatToHalf(u32 f) {
        const u32 v = Val(OpCompositeConstruct, tV2F, {f, CF(0.0f)});
        return And(Ext(tU32, GlslPackHalf2x16, {v}), CU(0xFFFF));
    }
    // Extension de signo de los 'bits' bajos (bits < 32)
    u32 SignExtend(u32 v, u32 bits) {
        if (bits >= 32) return v;
        return Val(OpBitFieldSExtract, tU32, {v, CU(0), CU(bits)});
    }

    // 64 bits
    u32 To64(u32 v, bool sgn) {
        return sgn ? Val(OpBitcast, tU64, {Val(OpSConvert, tS64, {AsS(v)})}) : Val(OpUConvert, tU64, {v});
    }
    u32 Lo(u32 v64) { return Val(OpUConvert, tU32, {v64}); }
    u32 Hi(u32 v64) { return Val(OpUConvert, tU32, {Val(OpShiftRightLogical, tU64, {v64, b.ConstU64(32)})}); }

    bool Need64(const char* what) {
        if (m_opt.int64) return true;
        Fail(std::string(what) + " necesita enteros de 64 bits (shaderInt64)");
        return false;
    }

    void EmitInstr(const ShaderInstr& in) {
        switch (in.op) {
            case ShOp::Nop: return;
            case ShOp::Mov: W(in.rd, B(in)); return;
            case ShOp::Mov32i: W(in.rd, CU(in.imm)); return;
            case ShOp::S2R: {
                const u32 sr = in.Bits(20, 8);
                // Pixeles: SR_Y_DIRECTION sale de la informacion del draw; lo demas, 0 (como el
                // dibujo por software). Modo de pruebas: como el TestEnv de los tests (todo 0).
                W(in.rd, Frag() && sr == 0x12 ? DrawInfo(0) : CU(0));
                return;
            }
            case ShOp::Sel: {
                u32 p = P(in.Bits(39, 3));
                if (in.Bit(42)) p = LNot(p);
                W(in.rd, Sel(p, A(in), B(in)));
                return;
            }

            // ---- Conversiones ----
            case ShOp::F2F: {
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const u32 bv = B(in);
                u32 v = src_size == 1 ? HalfToFloat(in.Bit(41) ? Shr(bv, CU(16)) : bv) : AsF(bv);
                if (in.Bit(44)) v = Ftz(v);
                v = AbsNeg(v, in.Bit(49), in.Bit(45));
                if (in.Bit(42)) v = RoundMode(v, in.Bits(39, 2));
                if (in.Bit(50)) v = Sat(v);
                W(in.rd, dst_size == 1 ? FloatToHalf(v) : AsU(v));
                return;
            }
            case ShOp::F2I: {
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const bool sgn = in.Bit(12);
                if (dst_size == 3) { Fail("F2I a 64 bits no soportado"); return; }
                const u32 bv = B(in);
                u32 v = src_size == 1 ? HalfToFloat(bv) : AsF(bv);
                if (in.Bit(44)) v = Ftz(v);
                v = AbsNeg(v, in.Bit(49), in.Bit(45));
                const u32 r = RoundMode(v, in.Bits(39, 2));
                const u32 bits = 8u << dst_size;
                // Limites del tipo destino (en float; los extremos se comparan antes de convertir)
                u32 out;
                if (sgn) {
                    const float lo = -std::ldexp(1.0f, int(bits) - 1), hi = std::ldexp(1.0f, int(bits) - 1);
                    const u32 too_lo = Val(OpFOrdLessThan, tBool, {r, CF(lo)});
                    const u32 too_hi = Val(OpFOrdGreaterThanEqual, tBool, {r, CF(hi)});
                    const u32 safe = Ext(tF32, GlslNClamp, {r, CF(lo), CF(bits == 32 ? 2147483520.0f : hi - 1)});
                    u32 conv = SToU(Val(OpConvertFToS, tS32, {safe}));
                    conv = Sel(too_lo, CU(u32(s32(-(s64(1) << (bits - 1))))), conv);
                    conv = Sel(too_hi, CU(u32((s64(1) << (bits - 1)) - 1)), conv);
                    out = conv;
                } else {
                    const float hi = std::ldexp(1.0f, int(bits));
                    const u32 too_hi = Val(OpFOrdGreaterThanEqual, tBool, {r, CF(hi)});
                    const u32 safe = Ext(tF32, GlslNClamp, {r, CF(0.0f), CF(bits == 32 ? 4294967040.0f : hi - 1)});
                    u32 conv = Val(OpConvertFToU, tU32, {safe});
                    conv = Sel(too_hi, CU(bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1), conv);
                    out = conv;
                }
                out = Sel(IsNan(v), CU(0), out);
                W(in.rd, out);
                SetCcInt(out);
                return;
            }
            case ShOp::I2F: {
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const bool sgn = in.Bit(13);
                if (src_size == 3) { Fail("I2F de 64 bits no soportado"); return; }
                u32 bv = B(in);
                const u32 sel = in.Bits(41, 2) * 8;
                if (sel) bv = Shr(bv, CU(sel));
                const u32 bits = 8u << src_size;
                if (bits < 32) bv = sgn ? SignExtend(bv, bits) : And(bv, CU((1u << bits) - 1));
                u32 f = sgn ? Val(OpConvertSToF, tF32, {AsS(bv)}) : Val(OpConvertUToF, tF32, {bv});
                f = AbsNeg(f, in.Bit(49), in.Bit(45));
                W(in.rd, dst_size == 1 ? FloatToHalf(f) : AsU(f));
                return;
            }
            case ShOp::I2I: {
                if (!Need64("I2I")) return;
                const u32 src_size = in.Bits(10, 2), dst_size = in.Bits(8, 2);
                const bool ssgn = in.Bit(13), dsgn = in.Bit(12);
                u32 bv = B(in);
                const u32 sel = in.Bits(41, 2) * 8;
                if (sel) bv = Shr(bv, CU(sel));
                const u32 sbits = 8u << std::min(src_size, 2u);
                if (sbits < 32) bv = ssgn ? SignExtend(bv, sbits) : And(bv, CU((1u << sbits) - 1));
                u32 v = Val(OpBitcast, tS64, {To64(bv, ssgn)});
                const u32 zero64 = Val(OpBitcast, tS64, {b.ConstU64(0)});
                if (in.Bit(49)) v = Sel(Val(OpSLessThan, tBool, {v, zero64}), Val(OpSNegate, tS64, {v}), v, tS64);
                if (in.Bit(45)) v = Val(OpSNegate, tS64, {v});
                const u32 dbits = 8u << std::min(dst_size, 2u);
                if (in.Bit(50)) {
                    const s64 lo = dsgn ? -(s64(1) << (dbits - 1)) : 0;
                    const s64 hi = dsgn ? (s64(1) << (dbits - 1)) - 1 : (s64(1) << dbits) - 1;
                    const u32 clo = Val(OpBitcast, tS64, {b.ConstU64(u64(lo))}), chi = Val(OpBitcast, tS64, {b.ConstU64(u64(hi))});
                    v = Sel(Val(OpSLessThan, tBool, {v, clo}), clo, v, tS64);
                    v = Sel(Val(OpSGreaterThan, tBool, {v, chi}), chi, v, tS64);
                }
                u32 out = Lo(Val(OpBitcast, tU64, {v}));
                if (dbits < 32) out = dsgn ? SignExtend(out, dbits) : And(out, CU((1u << dbits) - 1));
                W(in.rd, out);
                SetCcInt(out);
                return;
            }

            // ---- Coma flotante ----
            case ShOp::Fadd: case ShOp::Fadd32i: {
                const bool l = in.op == ShOp::Fadd32i;
                const bool ftz = in.Bit(l ? 55 : 44);
                u32 a = AbsNeg(AsF(A(in)), in.Bit(l ? 54 : 46), in.Bit(l ? 56 : 48));
                u32 c = AbsNeg(AsF(B(in)), in.Bit(l ? 57 : 49), in.Bit(l ? 53 : 45));
                if (ftz) { a = Ftz(a); c = Ftz(c); }
                u32 r = FAdd(a, c);
                if (ftz) r = Ftz(r);
                if (!l && in.Bit(50)) r = Sat(r);
                W(in.rd, AsU(r));
                return;
            }
            case ShOp::Fmul: case ShOp::Fmul32i: {
                const bool l = in.op == ShOp::Fmul32i;
                const u32 fmz = in.Bits(l ? 53 : 44, 2);
                u32 a = AsF(A(in)), c = AsF(B(in));
                if (!l && in.Bit(48)) a = FNeg(a);
                if (fmz) { a = Ftz(a); c = Ftz(c); }
                u32 r = FMul(a, c);
                if (fmz & 2) {
                    const u32 zero = LOr(Val(OpFOrdEqual, tBool, {a, CF(0.0f)}), Val(OpFOrdEqual, tBool, {c, CF(0.0f)}));
                    r = Sel(zero, CF(0.0f), r, tF32);
                }
                if (!l) {
                    const u32 pdiv = in.Bits(41, 3);
                    if (pdiv >= 1 && pdiv <= 3) r = FMul(r, CF(std::ldexp(1.0f, -int(pdiv))));
                    else if (pdiv >= 4 && pdiv <= 6) r = FMul(r, CF(std::ldexp(1.0f, int(7 - pdiv))));
                }
                if (fmz) r = Ftz(r);
                if (in.Bit(l ? 55 : 50)) r = Sat(r);
                W(in.rd, AsU(r));
                return;
            }
            case ShOp::Ffma: case ShOp::Ffma32i: {
                const bool l = in.op == ShOp::Ffma32i;
                const u32 fmz = in.Bits(53, 2);
                u32 a = AsF(A(in)), c = AsF(B(in));
                u32 d = AsF(l ? R(in.rd) : C(in));
                if (in.Bit(l ? 56 : 48)) a = FNeg(a);
                if (in.Bit(l ? 57 : 49)) d = FNeg(d);
                if (fmz) { a = Ftz(a); c = Ftz(c); d = Ftz(d); }
                u32 r = Ext(tF32, GlslFma, {a, c, d});
                if (fmz & 2) {
                    const u32 zero = LOr(Val(OpFOrdEqual, tBool, {a, CF(0.0f)}), Val(OpFOrdEqual, tBool, {c, CF(0.0f)}));
                    r = Sel(zero, d, r, tF32);
                }
                if (fmz) r = Ftz(r);
                if (in.Bit(l ? 55 : 50)) r = Sat(r);
                W(in.rd, AsU(r));
                return;
            }
            case ShOp::Mufu: {
                const u32 a = AbsNeg(AsF(A(in)), in.Bit(46), in.Bit(48));
                u32 r;
                switch (in.Bits(20, 4)) {
                    case 0: r = Ext(tF32, GlslCos, {a}); break;
                    case 1: r = Ext(tF32, GlslSin, {a}); break;
                    case 2: r = Ext(tF32, GlslExp2, {a}); break;
                    case 3: r = Ext(tF32, GlslLog2, {a}); break;
                    case 4: r = Val(OpFDiv, tF32, {CF(1.0f), a}); break;
                    case 5: r = Ext(tF32, GlslInverseSqrt, {a}); break;
                    case 8: r = Ext(tF32, GlslSqrt, {a}); break;
                    default: Fail("MUFU no soportado"); return;
                }
                if (in.Bit(50)) r = Sat(r);
                W(in.rd, AsU(r));
                return;
            }
            case ShOp::Rro:
                W(in.rd, AsU(AbsNeg(AsF(B(in)), in.Bit(49), in.Bit(45))));
                return;
            case ShOp::Fmnmx: {
                u32 a = AbsNeg(AsF(A(in)), in.Bit(46), in.Bit(48));
                u32 c = AbsNeg(AsF(B(in)), in.Bit(49), in.Bit(45));
                if (in.Bit(44)) { a = Ftz(a); c = Ftz(c); }
                u32 p = P(in.Bits(39, 3));
                if (in.Bit(42)) p = LNot(p);
                const u32 mn = Ext(tF32, GlslNMin, {a, c}), mx = Ext(tF32, GlslNMax, {a, c});
                W(in.rd, AsU(Sel(p, mn, mx, tF32)));
                return;
            }
            case ShOp::Fset: {
                u32 a = AbsNeg(AsF(A(in)), in.Bit(54), in.Bit(43));
                u32 c = AbsNeg(AsF(B(in)), in.Bit(44), in.Bit(53));
                if (in.Bit(55)) { a = Ftz(a); c = Ftz(c); }
                u32 p = P(in.Bits(39, 3));
                if (in.Bit(42)) p = LNot(p);
                const u32 r = BoolOp(in.Bits(45, 2), FCompare(in.Bits(48, 4), a, c), p);
                W(in.rd, Sel(r, CU(in.Bit(52) ? 0x3F800000u : 0xFFFFFFFFu), CU(0)));
                return;
            }
            case ShOp::Fsetp: {
                u32 a = AbsNeg(AsF(A(in)), in.Bit(7), in.Bit(43));
                u32 c = AbsNeg(AsF(B(in)), in.Bit(44), in.Bit(6));
                if (in.Bit(47)) { a = Ftz(a); c = Ftz(c); }
                u32 p = P(in.Bits(39, 3));
                if (in.Bit(42)) p = LNot(p);
                const u32 cmp = FCompare(in.Bits(48, 4), a, c);
                const u32 bop = in.Bits(45, 2);
                const u32 r1 = BoolOp(bop, cmp, p), r2 = BoolOp(bop, LNot(cmp), p);
                WP(in.Bits(3, 3), r1);
                WP(in.Bits(0, 3), r2);
                return;
            }
            case ShOp::Fcmp: {
                u32 c = AsF(C(in));
                if (in.Bit(47)) c = Ftz(c);
                W(in.rd, Sel(FCompare(in.Bits(48, 4), c, CF(0.0f)), A(in), B(in)));
                return;
            }

            // ---- Enteros ----
            case ShOp::Lop: case ShOp::Lop32i: {
                const bool l = in.op == ShOp::Lop32i;
                u32 a = A(in), c = B(in);
                if (in.Bit(l ? 55 : 39)) a = Not(a);
                if (in.Bit(l ? 56 : 40)) c = Not(c);
                u32 r;
                switch (in.Bits(l ? 53 : 41, 2)) {
                    case 0: r = And(a, c); break;
                    case 1: r = Or(a, c); break;
                    case 2: r = Xor(a, c); break;
                    default: r = c; break;
                }
                W(in.rd, r);
                if (!l) WP(in.Bits(48, 3), Ne(r, CU(0)));
                SetCcInt(r);
                return;
            }
            case ShOp::Iadd: case ShOp::Iadd32i: {
                if (!Need64("IADD")) return;
                const bool l = in.op == ShOp::Iadd32i;
                u32 a = A(in), c = B(in);
                const bool na = in.Bit(l ? 56 : 49), nb = l ? in.Bit(55) : in.Bit(48);
                u32 extra = 0;
                if (na && nb) extra = 1;
                else if (na) a = Neg(a);
                else if (nb) c = Neg(c);
                const bool x = in.Bit(l ? 53 : 43);
                const u32 cin = x ? Sel(Load(tBool, CcVar(2)), CU(1), CU(0)) : CU(0);
                const u32 sum = Val(OpIAdd, tU64, {Val(OpIAdd, tU64, {To64(a, false), To64(c, false)}),
                                                   To64(Add(CU(extra), cin), false)});
                u32 r = Lo(sum);
                if (in.Bit(l ? 54 : 50)) {   // .SAT con signo
                    const u32 s = Val(OpIAdd, tS64, {Val(OpIAdd, tS64, {Val(OpBitcast, tS64, {To64(a, true)}),
                                                                        Val(OpBitcast, tS64, {To64(c, true)})}),
                                                     Val(OpBitcast, tS64, {To64(cin, false)})});
                    const u32 lo = Val(OpBitcast, tS64, {b.ConstU64(u64(s64(INT32_MIN)))});
                    const u32 hi = Val(OpBitcast, tS64, {b.ConstU64(u64(s64(INT32_MAX)))});
                    u32 cl = Sel(Val(OpSLessThan, tBool, {s, lo}), lo, s, tS64);
                    cl = Sel(Val(OpSGreaterThan, tBool, {cl, hi}), hi, cl, tS64);
                    r = Lo(Val(OpBitcast, tU64, {cl}));
                }
                if (in.Bit(l ? 52 : 47)) {   // .CC
                    Store(CcVar(2), Ne(And(Hi(sum), CU(1)), CU(0)));
                    Store(CcVar(3), Ne(Shr(And(Not(Xor(a, c)), Xor(a, r)), CU(31)), CU(0)));
                    SetCcInt(r);
                }
                W(in.rd, r);
                return;
            }
            case ShOp::Imul: case ShOp::Imul32i: {
                if (!Need64("IMUL")) return;
                const bool l = in.op == ShOp::Imul32i;
                const bool sa = in.Bit(l ? 54 : 40), sb = in.Bit(l ? 55 : 41), hi = in.Bit(l ? 53 : 39);
                const u32 p = Val(OpIMul, tU64, {To64(A(in), sa), To64(B(in), sb)});
                W(in.rd, hi ? Hi(p) : Lo(p));
                return;
            }
            case ShOp::Imad: {
                if (!Need64("IMAD")) return;
                const bool hi = in.Bit(54), sgn = in.Bit(53) || in.Bit(48);
                const u32 a = A(in), c = B(in), d = C(in);
                u32 prod = Val(OpIMul, tU64, {To64(a, sgn), To64(c, sgn)});
                if (in.Bit(52)) prod = Val(OpISub, tU64, {b.ConstU64(0), prod});
                u32 r = hi ? Hi(prod) : Lo(prod);
                r = Add(r, in.Bit(51) ? Neg(d) : d);
                W(in.rd, r);
                return;
            }
            case ShOp::Iscadd: {
                u32 a = A(in), c = B(in);
                u32 extra = 0;
                if (in.Bit(49) && in.Bit(48)) extra = 1;
                else if (in.Bit(49)) a = Neg(a);
                else if (in.Bit(48)) c = Neg(c);
                const u32 r = Add(Add(Shl(a, CU(in.Bits(39, 5))), c), CU(extra));
                W(in.rd, r);
                SetCcInt(r);
                return;
            }
            case ShOp::Xmad: {
                bool psl = false, mrg = false, h1a = in.Bit(53), h1b = false;
                u32 cmode;
                switch (in.form) {
                    case ShForm::Reg:  psl = in.Bit(36); mrg = in.Bit(37); h1b = in.Bit(35); cmode = in.Bits(50, 3); break;
                    case ShForm::Imm:  psl = in.Bit(36); mrg = in.Bit(37); cmode = in.Bits(50, 3); break;
                    case ShForm::Cbuf: psl = in.Bit(55); mrg = in.Bit(56); h1b = in.Bit(52); cmode = in.Bits(50, 2); break;
                    default:           h1b = in.Bit(52); cmode = in.Bits(50, 2); break;
                }
                const bool sa = in.Bit(48), sb = in.Bit(49);
                const u32 a = A(in), c = B(in), d = C(in);
                auto half = [&](u32 v, bool high, bool sgn) {
                    const u32 h = high ? Shr(v, CU(16)) : And(v, CU(0xFFFF));
                    return sgn ? SignExtend(h, 16) : h;
                };
                const u32 ha = half(a, h1a, sa), hb = half(c, h1b, sb);
                u32 prod = Mul(ha, hb);
                if (psl) prod = Shl(prod, CU(16));
                u32 cc = d;
                switch (cmode) {
                    case 1: cc = And(d, CU(0xFFFF)); break;
                    case 2: cc = Shr(d, CU(16)); break;
                    case 3: {
                        const u32 nz = LAnd(Ne(ha, CU(0)), Ne(hb, CU(0)));
                        if (sa) cc = Sel(LAnd(nz, Ne(Shr(ha, CU(31)), CU(0))), Sub(cc, CU(0x10000)), cc);
                        if (sb) cc = Sel(LAnd(nz, Ne(Shr(hb, CU(31)), CU(0))), Sub(cc, CU(0x10000)), cc);
                        break;
                    }
                    case 4: cc = Add(d, Shl(c, CU(16))); break;
                    default: break;
                }
                u32 r = Add(prod, cc);
                if (mrg) r = Or(And(r, CU(0xFFFF)), Shl(c, CU(16)));
                W(in.rd, r);
                SetCcInt(r);
                return;
            }
            case ShOp::Imnmx: {
                const bool sgn = in.Bit(48);
                const u32 a = A(in), c = B(in);
                u32 p = P(in.Bits(39, 3));
                if (in.Bit(42)) p = LNot(p);
                const u32 lt = sgn ? SLt(AsS(a), AsS(c)) : ULt(a, c);
                W(in.rd, Sel(p, Sel(lt, a, c), Sel(lt, c, a)));
                return;
            }
            case ShOp::Iset: {
                u32 p = P(in.Bits(39, 3));
                if (in.Bit(42)) p = LNot(p);
                const u32 r = BoolOp(in.Bits(45, 2), ICompare(in.Bits(49, 3), A(in), B(in), in.Bit(48)), p);
                W(in.rd, Sel(r, CU(in.Bit(44) ? 0x3F800000u : 0xFFFFFFFFu), CU(0)));
                return;
            }
            case ShOp::Isetp: {
                u32 p = P(in.Bits(39, 3));
                if (in.Bit(42)) p = LNot(p);
                const u32 cmp = ICompare(in.Bits(49, 3), A(in), B(in), in.Bit(48));
                const u32 bop = in.Bits(45, 2);
                const u32 r1 = BoolOp(bop, cmp, p), r2 = BoolOp(bop, LNot(cmp), p);
                WP(in.Bits(3, 3), r1);
                WP(in.Bits(0, 3), r2);
                return;
            }
            case ShOp::Icmp:
                W(in.rd, Sel(ICompare(in.Bits(49, 3), C(in), CU(0), in.Bit(48)), A(in), B(in)));
                return;
            case ShOp::Shl: {
                const u32 a = A(in), s = B(in);
                const u32 r = in.Bit(39) ? Shl(a, And(s, CU(31))) : ShlSafe(a, s);
                W(in.rd, r);
                SetCcInt(r);
                return;
            }
            case ShOp::Shr: {
                const u32 a = A(in);
                u32 s = B(in);
                if (in.Bit(39)) s = And(s, CU(31));
                const u32 r = in.Bit(48) ? Sar(a, UMin(s, CU(31))) : ShrSafe(a, s);
                W(in.rd, r);
                SetCcInt(r);
                return;
            }
            case ShOp::Popc: {
                u32 v = B(in);
                if (in.Bit(40)) v = Not(v);
                W(in.rd, Val(OpBitCount, tU32, {v}));
                return;
            }
            case ShOp::Bfi: {
                const u32 a = A(in), c = B(in), d = C(in);
                const u32 pos = And(c, CU(0xFF)), len = And(Shr(c, CU(8)), CU(0xFF));
                const u32 ok = LAnd(Ne(len, CU(0)), ULt(pos, CU(32)));
                const u32 pos_s = UMin(pos, CU(31));
                const u32 n = UMin(len, Sub(CU(32), pos_s));
                const u32 mask = Shl(MaskBits(n), pos_s);
                const u32 r = Or(And(d, Not(mask)), And(Shl(a, pos_s), mask));
                const u32 out = Sel(ok, r, d);
                W(in.rd, out);
                SetCcInt(out);
                return;
            }
            case ShOp::Bfe: {
                u32 a = A(in);
                const u32 c = B(in);
                if (in.Bit(40)) a = Val(OpBitReverse, tU32, {a});
                const u32 pos = And(c, CU(0xFF)), len = And(Shr(c, CU(8)), CU(0xFF));
                const bool sgn = in.Bit(48);
                const u32 neg = sgn ? Ne(Shr(a, CU(31)), CU(0)) : CB(false);
                const u32 pos_s = UMin(pos, CU(31));
                const u32 n = UMin(len, Sub(CU(32), pos_s));
                const u32 mask = MaskBits(n);
                u32 r = And(Shr(a, pos_s), mask);
                if (sgn) {
                    const u32 n_lt32 = ULt(n, CU(32));
                    const u32 topbit = Ne(And(r, ShlSafe(CU(1), Sub(n, CU(1)))), CU(0));
                    r = Sel(LAnd(n_lt32, topbit), Or(r, Not(mask)), r);
                    const u32 beyond = LAnd(UGe(len, Sub(CU(33), pos_s)), neg);   // len > 32 - pos
                    r = Sel(beyond, Or(r, Not(mask)), r);
                }
                r = Sel(UGe(pos, CU(32)), Sel(neg, CU(0xFFFFFFFFu), CU(0)), r);
                r = Sel(Eq(len, CU(0)), CU(0), r);
                W(in.rd, r);
                SetCcInt(r);
                return;
            }
            case ShOp::Flo: {
                u32 v = B(in);
                if (in.Bit(40)) v = Not(v);
                if (in.Bit(48)) v = Sel(SLt(AsS(v), AsS(CU(0))), Not(v), v);
                u32 r = Ext(tU32, GlslFindUMsb, {v});   // 0 -> -1 (0xFFFFFFFF), como el interprete
                if (in.Bit(41)) r = Sel(Eq(r, CU(0xFFFFFFFFu)), r, Sub(CU(31), r));
                W(in.rd, r);
                return;
            }

            // ---- Atributos y memoria ----
            case ShOp::Ald: {
                const u32 count = in.Bits(47, 2) + 1;
                const u32 addr = Add(CU(in.Bits(20, 10)), R(in.ra));
                if (Vert()) {   // direccion fija (ScanVertex)
                    for (u32 i = 0; i < count; ++i) W(in.rd + i, VertexInput(in.Bits(20, 10) + 4 * i));
                    return;
                }
                for (u32 i = 0; i < count; ++i) {
                    const u32 a = Add(addr, CU(4 * i));
                    W(in.rd + i, Frag() ? FragAttributeBits(a) : ReadAttribute(a));
                }
                return;
            }
            case ShOp::Ast: {
                if (Frag()) return;   // el shader de pixeles no tiene atributos de salida
                const u32 count = in.Bits(47, 2) + 1;
                if (Vert()) {
                    for (u32 i = 0; i < count; ++i) {
                        const u32 a = in.Bits(20, 10) + 4 * i;
                        if (a < 0x400) Store(OutWord(a / 4), R(in.rd + i));
                    }
                    return;
                }
                const u32 addr = Add(CU(in.Bits(20, 10)), R(in.ra));
                for (u32 i = 0; i < count; ++i) WriteAttribute(Add(addr, CU(4 * i)), R(in.rd + i));
                return;
            }
            case ShOp::Ipa: {
                const u32 addr = Add(CU(in.Bits(28, 10)), in.Bit(38) ? R(in.ra) : CU(0));
                const u32 mode = in.Bits(54, 2);
                u32 v = Frag() ? FragInput(in.Bits(28, 10), mode)
                               : AsF(ReadAttribute(addr));   // modo de pruebas: el valor del atributo tal cual
                if (mode == 1 || (mode == 3 && in.Bits(20, 8) != 0xFF)) v = FMul(v, AsF(R(in.Bits(20, 8))));
                if (in.Bit(51)) v = Sat(v);
                W(in.rd, AsU(v));
                return;
            }
            case ShOp::Ldc: {
                const u32 size = in.Bits(48, 3);
                const u32 idx = in.Bits(36, 5);
                const u32 addr = Add(CU(u32(s32(s16(u16(in.Bits(20, 16)))))), R(in.ra));
                const u32 words = size == 5 ? 2 : size == 6 ? 4 : 1;
                if (idx < 18) {
                    const s32 off = s32(s16(u16(in.Bits(20, 16))));
                    if (in.ra == 0xFF && off >= 0) m_cbufEnd[idx] = std::max(m_cbufEnd[idx], (u32(off) & ~3u) + 4 * words);
                    else m_cbufDynamic |= 1u << idx;
                }
                for (u32 i = 0; i < words; ++i) {
                    u32 v = ConstDynamic(idx, Add(And(addr, CU(~3u)), CU(4 * i)));
                    if (size < 4) {
                        v = Shr(v, Shl(And(addr, CU(3)), CU(3)));
                        if (size == 0) v = And(v, CU(0xFF));
                        else if (size == 1) v = SignExtend(v, 8);
                        else if (size == 2) v = And(v, CU(0xFFFF));
                        else v = SignExtend(v, 16);
                    }
                    W(in.rd + i, v);
                }
                return;
            }
            case ShOp::Ldl: case ShOp::Stl: {
                const u32 size = in.Bits(48, 3);
                if (size < 4) { Fail("LDL/STL de menos de 32 bits no soportado"); return; }
                const u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
                const u32 addr = Add(CU(u32(s32(in.Bits(20, 24) << 8) >> 8)), R(in.ra));
                if (!m_local) {
                    const u32 arr = b.TypeArray(tU32, CU(LOCAL_WORDS));
                    m_local = FnVar(b.TypePointer(Function, arr), 0);
                }
                for (u32 i = 0; i < words; ++i) {
                    const u32 word = Add(Shr(addr, CU(2)), CU(i));
                    const u32 inside = ULt(word, CU(LOCAL_WORDS));
                    const u32 safe = Sel(inside, word, CU(0));
                    const u32 ptr = Val(OpAccessChain, tPtrFnU32, {m_local, safe});
                    if (in.op == ShOp::Stl) {
                        const u32 v = R(in.rd + i);
                        If(inside, [&] { Store(ptr, v); });
                    } else {
                        W(in.rd + i, Sel(inside, Load(tU32, ptr), CU(0)));
                    }
                }
                return;
            }

            case ShOp::Tex: case ShOp::Texs: case ShOp::Tlds: case ShOp::Txd: case ShOp::Txq:
                if (!Frag()) { Fail(std::string("texturas fuera del shader de pixeles (") + ShOpName(in.op) + ")"); return; }
                EmitTexture(in);
                return;
            default:
                Fail(std::string("instruccion no traducida: ") + ShOpName(in.op));
                return;
        }
    }

    // Atributos (modo de pruebas: arrays en storage buffers; a[addr] = palabra addr/4)
    u32 ReadAttribute(u32 addr) {
        const u32 word = And(Shr(addr, CU(2)), CU(0xFF));
        const u32 ptr = Val(OpAccessChain, b.TypePointer(StorageBuffer, tU32), {m_attrIn, CU(0), word});
        return Load(tU32, ptr);
    }
    void WriteAttribute(u32 addr, u32 v) {
        const u32 word = And(Shr(addr, CU(2)), CU(0xFF));
        const u32 ptr = Val(OpAccessChain, b.TypePointer(StorageBuffer, tU32), {m_attrOut, CU(0), word});
        Store(ptr, v);
    }

    // ---- Shader de vertices ----
    u32 StorageWord(u32 var, u32 index) {
        return Load(tU32, Val(OpAccessChain, b.TypePointer(StorageBuffer, tU32), {var, CU(0), index}));
    }
    // a[addr] de entrada: atributo del vertice (de VS_ATTR), su id o el de la instancia
    u32 VertexInput(u32 addr) {
        if (addr >= 0x80 && addr < 0x280) {
            const u32 loc = (addr - 0x80) / 16;
            const u32 rank = u32(std::popcount(m_vsAttrMask & ((1u << loc) - 1)));
            return StorageWord(m_vsAttr, Add(VertexBase(), CU(4 * rank + (addr / 4) % 4)));
        }
        if (addr == 0x2FC) return StorageWord(m_vsAttr, Add(VertexBase(), CU(4 * u32(std::popcount(m_vsAttrMask)))));
        if (addr == 0x2F8) return StorageWord(m_vsInfo, CU(9));
        return CU(0);
    }
    u32 VertexBase() {
        if (!m_vertexBase) {
            // Se calcula en el bloque de entrada (antes del bucle) y se guarda en una variable
            m_vertexBase = FnVar(tPtrFnU32, 0);
            m_vertexBaseInit = true;
        }
        return Load(tU32, m_vertexBase);
    }
    // Palabra 'word' de las salidas (a[word * 4]); sin escribir: 0 (w de la posicion: 1)
    u32 OutWord(u32 word) {
        auto it = m_outWords.find(word);
        if (it != m_outWords.end()) return it->second;
        const u32 v = FnVar(tPtrFnU32, CU(word == 0x7C / 4 ? 0x3F800000u : 0u));
        m_outWords[word] = v;
        return v;
    }
    u32 OutValue(u32 word) {
        auto it = m_outWords.find(word);
        if (it != m_outWords.end()) return Load(tU32, it->second);
        return CU(word == 0x7C / 4 ? 0x3F800000u : 0u);
    }
    // Al terminar: posicion con el viewport (como el dibujo con Vulkan en la CPU), planos y varyings
    void EmitVertexOutputs() {
        const u32 x = AsF(OutValue(0x70 / 4)), y = AsF(OutValue(0x74 / 4)), z = AsF(OutValue(0x78 / 4)), w = AsF(OutValue(0x7C / 4));
        auto info = [&](u32 i) { return AsF(StorageWord(m_vsInfo, CU(i))); };
        const u32 sx = info(0), sy = info(1), sz = info(2), ox = info(3), oy = info(4), oz = info(5), fw = info(6), fh = info(7);
        const u32 zero_to_one = Ne(StorageWord(m_vsInfo, CU(8)), CU(0));
        auto fdiv = [&](u32 a, u32 c) { return Val(OpFDiv, tF32, {a, c}); };
        auto fsub = [&](u32 a, u32 c) { return Val(OpFSub, tF32, {a, c}); };
        const u32 px = fsub(fdiv(FMul(CF(2.0f), FAdd(FMul(sx, x), FMul(ox, w))), fw), w);
        const u32 py = fsub(fdiv(FMul(CF(2.0f), FAdd(FMul(sy, y), FMul(oy, w))), fh), w);
        const u32 pz = FAdd(FMul(sz, z), FMul(oz, w));
        Store(m_posOut, Val(OpCompositeConstruct, tV4F, {px, py, pz, w}));
        const u32 tPtrOutF = b.TypePointer(Output, tF32);
        Store(Val(OpAccessChain, tPtrOutF, {m_clipOut, CU(0)}), Sel(zero_to_one, z, FAdd(z, w), tF32));
        Store(Val(OpAccessChain, tPtrOutF, {m_clipOut, CU(1)}), fsub(w, z));
        for (size_t i = 0; i < m_opt.vs_outputs.size(); ++i) {
            const u32 slot = m_opt.vs_outputs[i].slot;
            u32 c[4];
            for (u32 k = 0; k < 4; ++k) c[k] = OutValue(slot * 4 + k);
            if (m_opt.vs_outputs[i].kind == SpirvVarying::Flat) Store(m_varVars[i], Val(OpCompositeConstruct, tV4U, {c[0], c[1], c[2], c[3]}));
            else Store(m_varVars[i], Val(OpCompositeConstruct, tV4F, {AsF(c[0]), AsF(c[1]), AsF(c[2]), AsF(c[3])}));
        }
    }

    // ---- Shader de pixeles ----
    // Palabra i de la informacion del draw (binding DRAW_INFO)
    u32 DrawInfo(u32 i) {
        if (!m_drawInfo) m_drawInfo = StorageArray(SpirvBinding::DRAW_INFO, "draw_info", true);
        return Load(tU32, Val(OpAccessChain, b.TypePointer(StorageBuffer, tU32), {m_drawInfo, CU(0), CU(i)}));
    }
    u32 FragCoord(u32 c) { return Val(OpCompositeExtract, tF32, {Load(tV4F, m_fragCoord), c}); }
    // IPA: atributo 'addr' (fijo) interpolado en el pixel, como PixelEnv::Interpolate
    u32 FragInput(u32 addr, u32 mode) {
        if (addr >= 0x70 && addr < 0x80) return FragCoord((addr - 0x70) / 4);
        if (addr == 0x3FC) return Sel(Load(tBool, m_front), CF(1.0f), CF(0.0f), tF32);
        if (addr >= 0x400) return CF(0.0f);
        const auto kind = mode == 2 ? SpirvVarying::Flat : mode == 0 ? SpirvVarying::Linear : SpirvVarying::Perspective;
        const s32 i = VaryingIndex(addr / 16, kind);
        if (i < 0) { Fail("entrada de IPA sin declarar"); return CF(0.0f); }
        if (kind == SpirvVarying::Flat)
            return AsF(Val(OpCompositeExtract, tU32, {Load(tV4U, m_varVars[i]), (addr / 4) & 3}));
        const u32 v = Val(OpCompositeExtract, tF32, {Load(tV4F, m_varVars[i]), (addr / 4) & 3});
        return kind == SpirvVarying::Perspective ? FMul(v, FragCoord(3)) : v;
    }
    // ALD en el shader de pixeles (direccion calculada): gl_FragCoord, gl_FrontFacing o 0
    u32 FragAttributeBits(u32 addr) {
        u32 v = Sel(Load(tBool, m_front), CU(0xFFFFFFFFu), CU(0));
        v = Sel(Eq(addr, CU(0x3FC)), v, CU(0));
        for (u32 c = 0; c < 4; ++c) v = Sel(Eq(addr, CU(0x70 + 4 * c)), AsU(FragCoord(c)), v);
        return v;
    }
    // Al terminar: colores desde los registros (R0.. segun la cabecera) y profundidad
    void EmitFragmentOutputs() {
        const ShaderHeader& h = m_prog.Header();
        s32 out_reg[8][4];
        s32 reg = 0;
        for (u32 t = 0; t < 8; ++t)
            for (u32 c = 0; c < 4; ++c) out_reg[t][c] = ((h.OmapTarget() >> (4 * t + c)) & 1) ? reg++ : -1;
        for (u32 k = 0; k < m_colorVars.size(); ++k) {
            const u32 o = m_opt.mrt ? k : 0;
            const u8 kind = m_opt.color_int[k];
            u32 parts[4];
            for (u32 c = 0; c < 4; ++c) {
                const s32 r = out_reg[o][c];
                u32 bits = r >= 0 ? R(u32(r)) : CU(c == 3 ? (kind ? 1u : 0x3F800000u) : 0u);
                parts[c] = kind == 1 ? AsS(bits) : kind == 2 ? bits : AsF(bits);
            }
            const u32 type = kind == 1 ? tV4S : kind == 2 ? tV4U : tV4F;
            Store(m_colorVars[k], Val(OpCompositeConstruct, type, {parts[0], parts[1], parts[2], parts[3]}));
        }
        if (m_fragDepth) Store(m_fragDepth, Sat(AsF(R(u32(reg + 1)))));
    }

    // ---- Texturas ----
    u32 FVec(const std::vector<u32>& c) {
        if (c.size() == 1) return c[0];
        std::vector<u32> ops{b.TypeVector(tF32, u32(c.size())), 0};
        ops[1] = b.Id();
        ops.insert(ops.end(), c.begin(), c.end());
        E(OpCompositeConstruct, ops);
        return ops[1];
    }
    u32 SVec(const std::vector<u32>& c) {
        if (c.size() == 1) return c[0];
        std::vector<u32> ops{b.TypeVector(tS32, u32(c.size())), 0};
        ops[1] = b.Id();
        ops.insert(ops.end(), c.begin(), c.end());
        E(OpCompositeConstruct, ops);
        return ops[1];
    }
    // Resultado (vec4 float) -> 4 palabras
    void Unpack(u32 v4, u32 out[4]) {
        for (u32 c = 0; c < 4; ++c) out[c] = AsU(Val(OpCompositeExtract, tF32, {v4, c}));
    }
    void EmitTexture(const ShaderInstr& in) {
        const u32 slot = in.Bits(36, 13);
        const s32 k = TextureIndex(slot);
        if (k < 0) { Fail("textura sin declarar"); return; }
        const SpirvTexture& t = m_textures[size_t(k)];
        const u32 ncoord = t.dim == 0 ? 1 : t.dim == 1 ? 2 : 3;
        const u32 sampled = Load(m_texSim[size_t(k)], m_texVar[size_t(k)]);
        // Coordenadas en texeles (sampler sin normalizar): se multiplican por 1/tamano
        auto scale = [&](u32 f, u32 c) { return c < 2 ? FMul(f, AsF(DrawInfo(4 + 2 * u32(k) + c))) : f; };
        u32 val[4];

        if (in.op == ShOp::Txq) {
            if (in.Bits(22, 6) != 1) { Fail("consulta de textura no soportada (solo tamano)"); return; }
            const u32 img = Val(OpImage, m_texImg[size_t(k)], {sampled});
            const u32 n = ncoord + (t.array ? 1 : 0) - (t.dim == 3 ? 1 : 0);
            const u32 size = Val(OpImageQuerySizeLod, n == 1 ? tS32 : b.TypeVector(tS32, n), {img, R(in.ra)});
            for (u32 c = 0; c < 3; ++c)
                val[c] = c < n ? SToU(n == 1 ? size : Val(OpCompositeExtract, tS32, {size, c})) : CU(1);
            val[3] = SToU(Val(OpImageQueryLevels, tS32, {img}));
        } else if (in.op == ShOp::Texs || in.op == ShOp::Tlds) {
            static const char* const k_texs[16] = {"x", "xy", "xy", "xyl", "xyd", "xyld", "xyd", "Lxy",
                                                    "Lxy", "Lxyd", "xyz", "xyz", "xyz", "xyzl", nullptr, nullptr};
            static const char* const k_tlds[16] = {"x", "xl", "xy", nullptr, "xyo", "xyl", "xym", "xyz",
                                                    "Lxy", nullptr, nullptr, nullptr, "xylo", nullptr, nullptr, nullptr};
            static const bool k_texs_lz[16] = {true, false, true, false, false, false, true, false,
                                               true, true, false, true, false, false, false, false};
            const bool fetch = in.op == ShOp::Tlds;
            const u32 target = in.Bits(53, 4);
            const char* srcs = (fetch ? k_tlds : k_texs)[target];
            const u32 n = u32(std::strlen(srcs));
            const u32 ra = in.ra, rb = in.Bits(20, 8);
            const u32 regs[4] = {ra, n == 2 ? rb : ra + 1, rb, rb + 1};
            std::vector<u32> coords;
            u32 layer = 0, lod = 0, offset = 0;
            bool has_layer = false, has_lod = false, has_offset = false;
            for (u32 i = 0; i < n; ++i) {
                const u32 v = R(regs[i]);
                switch (srcs[i]) {
                    case 'x': case 'y': case 'z': coords.push_back(v); break;
                    case 'L': layer = And(v, CU(0xFFFF)); has_layer = true; break;
                    case 'l': lod = v; has_lod = true; break;
                    case 'o': offset = v; has_offset = true; break;
                    default: break;   // 'm': muestra (sin multisample)
                }
            }
            if (fetch) {
                // texelFetch: coordenadas enteras (+ offset), nivel entero
                std::vector<u32> ic;
                for (u32 c = 0; c < coords.size(); ++c) {
                    u32 v = AsS(coords[c]);
                    if (has_offset && c < 2)
                        v = Val(OpIAdd, tS32, {v, Val(OpBitFieldSExtract, tS32, {AsS(offset), CU(4 * c), CU(4)})});
                    ic.push_back(v);
                }
                if (has_layer) ic.push_back(AsS(layer));
                const u32 img = Val(OpImage, m_texImg[size_t(k)], {sampled});
                const u32 res = Val(OpImageFetch, tV4F, {img, SVec(ic), ImgLod, has_lod ? AsS(lod) : AsS(CU(0))});
                Unpack(res, val);
            } else {
                std::vector<u32> fc;
                for (u32 c = 0; c < coords.size(); ++c) fc.push_back(scale(AsF(coords[c]), c));
                if (has_layer) fc.push_back(Val(OpConvertUToF, tF32, {layer}));
                u32 res;
                if (k_texs_lz[target] || has_lod)
                    res = Val(OpImageSampleExplicitLod, tV4F, {sampled, FVec(fc), ImgLod, has_lod ? AsF(lod) : CF(0.0f)});
                else
                    res = Val(OpImageSampleImplicitLod, tV4F, {sampled, FVec(fc)});
                Unpack(res, val);
            }
            // Que componentes se escriben (como el interprete)
            static const u8 k_one[8] = {0x1, 0x2, 0x4, 0x8, 0x3, 0x9, 0xA, 0xC};
            static const u8 k_two[8] = {0x7, 0xB, 0xD, 0xE, 0xF, 0xF, 0xF, 0xF};
            const u32 rd = in.Bits(0, 8), rd2 = in.Bits(28, 8);
            const u32 mask = (rd2 == 0xFF ? k_one : k_two)[in.Bits(50, 3)];
            u32 outn = 0;
            for (u32 c = 0; c < 4; ++c) {
                if (!(mask & (1u << c))) continue;
                const u32 dst = outn < 2 ? rd + outn : rd2 + (outn - 2);
                if ((outn < 2 ? rd : rd2) != 0xFF) W(dst, val[c]);
                ++outn;
            }
            return;
        } else {
            // TEX / TXD: Ra.. = [capa] coordenadas; Rb.. = [lod o sesgo | derivadas] [offsets]
            const u32 lodm = in.op == ShOp::Tex ? in.Bits(55, 2) : 0;   // 0 auto, 1 lz, 2 lb, 3 ll
            const bool aoffi = in.op == ShOp::Tex && in.Bit(54);
            u32 a = in.ra, bb = in.Bits(20, 8);
            auto nexta = [&]() { const u32 v = R(a); if (a < 255) ++a; return v; };
            auto nextb = [&]() { const u32 v = R(bb); if (bb < 255) ++bb; return v; };
            u32 layer = 0;
            if (t.array) layer = And(nexta(), CU(0xFFFF));
            std::vector<u32> fc;
            for (u32 c = 0; c < ncoord; ++c) fc.push_back(scale(AsF(nexta()), c));
            if (t.array) fc.push_back(Val(OpConvertUToF, tF32, {layer}));
            std::vector<u32> ops{tV4F, b.Id(), sampled, FVec(fc)};
            u32 mask = 0;
            std::vector<u32> extra;
            u32 op = OpImageSampleImplicitLod;
            if (in.op == ShOp::Txd) {
                std::vector<u32> dx, dy;
                for (u32 c = 0; c < ncoord; ++c) { dx.push_back(scale(AsF(nextb()), c)); dy.push_back(scale(AsF(nextb()), c)); }
                op = OpImageSampleExplicitLod;
                mask |= ImgGrad;
                extra.push_back(FVec(dx));
                extra.push_back(FVec(dy));
            } else if (lodm == 1) {
                op = OpImageSampleExplicitLod; mask |= ImgLod; extra.push_back(CF(0.0f));
            } else if (lodm == 2) {
                mask |= ImgBias; extra.push_back(AsF(nextb()));
            } else if (lodm == 3) {
                op = OpImageSampleExplicitLod; mask |= ImgLod; extra.push_back(AsF(nextb()));
            }
            if (aoffi) {
                if (t.dim == 3) { Fail("offsets en un cubo"); return; }
                const u32 v = AsS(nextb());
                std::vector<u32> off;
                for (u32 c = 0; c < ncoord; ++c) off.push_back(Val(OpBitFieldSExtract, tS32, {v, CU(4 * c), CU(4)}));
                if (!m_gatherExt) { b.Capability(25); m_gatherExt = true; }   // ImageGatherExtended
                mask |= ImgOffset;
                extra.push_back(SVec(off));
            }
            if (mask) { ops.push_back(mask); ops.insert(ops.end(), extra.begin(), extra.end()); }
            E(op, ops);
            Unpack(ops[1], val);
        }
        const u32 wmask = in.Bits(31, 4);
        u32 d = in.rd;
        for (u32 c = 0; c < 4; ++c)
            if (wmask & (1u << c)) { W(d, val[c]); if (d < 255) ++d; }
    }

    ShaderProgram& m_prog;
    SpirvOptions m_opt;
    Builder b;
    std::string m_error;
    std::map<u32, ShaderInstr> m_instrs;
    std::set<u32> m_leaders;

    std::vector<u32> m_code, m_vars;
    std::vector<std::pair<u32, u32>> m_inits;
    std::vector<u32> m_interface;
    std::map<u32, u32> m_regs, m_preds, m_cbufs;
    u32 m_cc[4] = {};
    u32 m_cbufMask = 0, m_cbufDynamic = 0;
    std::array<u32, 18> m_cbufEnd{};
    // Shader de vertices
    u32 m_vsAttrMask = 0;
    bool m_vsVertexId = false;
    u32 m_vertexIndex = 0, m_posOut = 0, m_clipOut = 0, m_vsAttr = 0, m_vsInfo = 0;
    u32 m_vertexBase = 0;
    bool m_vertexBaseInit = false;
    std::map<u32, u32> m_outWords;
    bool m_arrayStrideDone = false;

    u32 m_glsl = 0;
    u32 tVoid = 0, tBool = 0, tU32 = 0, tS32 = 0, tF32 = 0, tV2F = 0, tU64 = 0, tS64 = 0;
    u32 tPtrFnU32 = 0, tPtrFnBool = 0;
    u32 m_attrIn = 0, m_attrOut = 0;
    u32 m_pcVar = 0, m_running = 0;
    u32 m_stackKind = 0, m_stackTarget = 0, m_sp = 0, m_found = 0, m_foundTarget = 0;
    u32 m_local = 0;
    // Shader de pixeles
    std::vector<SpirvVarying> m_varyings;
    std::vector<SpirvTexture> m_textures;
    bool m_needFragCoord = false, m_needFront = false, m_gatherExt = false;
    u32 tV4F = 0, tV4U = 0, tV4S = 0;
    std::vector<u32> m_varVars, m_colorVars, m_texImg, m_texSim, m_texVar;
    u32 m_fragCoord = 0, m_front = 0, m_fragDepth = 0, m_drawInfo = 0;
};

} // namespace

SpirvResult TranslateToSpirv(ShaderProgram& program, const SpirvOptions& options) {
    Translator t(program, options);
    return t.Run();
}

} // namespace NeXo2::GPU

namespace NeXo2::GPU {

std::vector<u32> BuildPassthroughVertexShader(const std::vector<SpirvVarying>& varyings) {
    using namespace Spv;
    Builder b;
    b.Capability(1);    // Shader
    b.Capability(32);   // ClipDistance
    Builder::Emit(b.memory_model, OpMemoryModel, {0, 1});
    const u32 tVoid = b.TypeVoid(), tF32 = b.TypeFloat(32), tU32 = b.TypeInt(32, false);
    const u32 tV4F = b.TypeVector(tF32, 4), tV4U = b.TypeVector(tU32, 4);
    std::vector<u32> iface;
    auto var = [&](u32 type, u32 storage) {
        const u32 v = b.GlobalVariable(b.TypePointer(storage, type), storage);
        iface.push_back(v);
        return v;
    };
    const u32 pos_in = var(tV4F, Input);
    b.Decorate(pos_in, DecLocation, {0});
    const u32 pos_out = var(tV4F, Output);
    b.Decorate(pos_out, DecBuiltIn, {BuiltInPosition});
    // Planos de recorte cercano y lejano, calculados en la CPU como en el dibujo por software
    const u32 tV2F = b.TypeVector(tF32, 2);
    const u32 clip_in = var(tV2F, Input);
    b.Decorate(clip_in, DecLocation, {1});
    const u32 tClip = b.TypeArray(tF32, b.ConstU32(2));
    const u32 clip_out = var(tClip, Output);
    b.Decorate(clip_out, DecBuiltIn, {3 /* ClipDistance */});
    std::vector<std::pair<u32, u32>> pairs;   // (entrada, salida)
    std::vector<u32> types;
    for (size_t i = 0; i < varyings.size(); ++i) {
        const bool flat = varyings[i].kind == SpirvVarying::Flat;
        const u32 type = flat ? tV4U : tV4F;
        const u32 in = var(type, Input), out = var(type, Output);
        b.Decorate(in, DecLocation, {u32(i + 2)});
        b.Decorate(out, DecLocation, {u32(i)});
        if (flat) b.Decorate(out, DecFlat);
        else if (varyings[i].kind == SpirvVarying::Linear) b.Decorate(out, DecNoPerspective);
        pairs.push_back({in, out});
        types.push_back(type);
    }
    const u32 fn = b.Id(), entry = b.Id();
    Builder::Emit(b.functions, OpFunction, {tVoid, fn, 0, b.TypeFunction(tVoid)});
    Builder::Emit(b.functions, OpLabel, {entry});
    auto copy = [&](u32 type, u32 in, u32 out) {
        const u32 v = b.Id();
        Builder::Emit(b.functions, OpLoad, {type, v, in});
        Builder::Emit(b.functions, OpStore, {out, v});
    };
    copy(tV4F, pos_in, pos_out);
    {
        const u32 c = b.Id();
        Builder::Emit(b.functions, OpLoad, {tV2F, c, clip_in});
        const u32 tPtrOutF = b.TypePointer(Output, tF32);
        for (u32 k = 0; k < 2; ++k) {
            const u32 v = b.Id(), ptr = b.Id();
            Builder::Emit(b.functions, OpCompositeExtract, {tF32, v, c, k});
            Builder::Emit(b.functions, OpAccessChain, {tPtrOutF, ptr, clip_out, b.ConstU32(k)});
            Builder::Emit(b.functions, OpStore, {ptr, v});
        }
    }
    for (size_t i = 0; i < pairs.size(); ++i) copy(types[i], pairs[i].first, pairs[i].second);
    Builder::Emit(b.functions, OpReturn, {});
    Builder::Emit(b.functions, OpFunctionEnd, {});
    std::vector<u32> ep{0 /* Vertex */, fn};
    Builder::PushString(ep, "main");
    ep.insert(ep.end(), iface.begin(), iface.end());
    Builder::Emit(b.entry_points, OpEntryPoint, ep);
    return b.Assemble();
}

} // namespace NeXo2::GPU
