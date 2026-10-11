#pragma once
// Escritor de SPIR-V muy pequeno (el "lenguaje maquina" de los shaders de Vulkan).
//
// Un modulo SPIR-V es una lista de palabras de 32 bits: cabecera y luego instrucciones,
// cada una con (numero de palabras << 16 | codigo) seguido de sus operandos. Los valores
// se identifican con "ids" (numeros). Las instrucciones van en secciones con un orden fijo
// (capacidades, tipos y constantes, funciones...), asi que se guardan por separado y se
// juntan al final en Assemble().
//
// Referencia: la especificacion de SPIR-V de Khronos (los numeros de cada instruccion
// estan en spirv.core.grammar.json) y "GLSL.std.450" para las funciones matematicas.
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include "common/types.hpp"

namespace NeXo2::GPU::Spv {

// Codigos de las instrucciones que usamos
enum Op : u32 {
    OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15,
    OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21,
    OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
    OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33,
    OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpArrayLength = 68, OpDecorate = 71, OpMemberDecorate = 72,
    OpCompositeConstruct = 80, OpCompositeExtract = 81,
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpImageSampleDrefExplicitLod = 90,
    OpImageFetch = 95, OpImage = 100, OpImageQuerySizeLod = 103, OpImageQueryLevels = 106,
    OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112,
    OpUConvert = 113, OpSConvert = 114, OpBitcast = 124,
    OpSNegate = 126, OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpISub = 130, OpFSub = 131,
    OpIMul = 132, OpFMul = 133, OpUDiv = 134, OpFDiv = 136, OpIAddCarry = 149,
    OpUMulExtended = 151, OpSMulExtended = 152, OpIsNan = 156,
    OpLogicalEqual = 164, OpLogicalNotEqual = 165, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168,
    OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpUGreaterThan = 172, OpSGreaterThan = 173,
    OpUGreaterThanEqual = 174, OpSGreaterThanEqual = 175, OpULessThan = 176, OpSLessThan = 177,
    OpULessThanEqual = 178, OpSLessThanEqual = 179,
    OpFOrdEqual = 180, OpFUnordEqual = 181, OpFOrdNotEqual = 182, OpFUnordNotEqual = 183,
    OpFOrdLessThan = 184, OpFUnordLessThan = 185, OpFOrdGreaterThan = 186, OpFUnordGreaterThan = 187,
    OpFOrdLessThanEqual = 188, OpFUnordLessThanEqual = 189, OpFOrdGreaterThanEqual = 190,
    OpFUnordGreaterThanEqual = 191,
    OpShiftRightLogical = 194, OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196,
    OpBitwiseOr = 197, OpBitwiseXor = 198, OpBitwiseAnd = 199, OpNot = 200,
    OpBitFieldInsert = 201, OpBitFieldSExtract = 202, OpBitFieldUExtract = 203, OpBitReverse = 204,
    OpBitCount = 205,
    OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250,
    OpSwitch = 251, OpKill = 252, OpReturn = 253, OpUnreachable = 255,
};

// Funciones de GLSL.std.450
enum Glsl : u32 {
    GlslRoundEven = 2, GlslTrunc = 3, GlslFAbs = 4, GlslFloor = 8, GlslCeil = 9,
    GlslSin = 13, GlslCos = 14, GlslExp2 = 29, GlslLog2 = 30, GlslSqrt = 31, GlslInverseSqrt = 32,
    GlslFMin = 37, GlslFMax = 40, GlslFma = 50, GlslPackHalf2x16 = 58, GlslUnpackHalf2x16 = 62,
    GlslFindUMsb = 75, GlslNMin = 79, GlslNMax = 80, GlslNClamp = 81,
};

enum StorageClass : u32 {
    UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Private = 6, Function = 7, StorageBuffer = 12,
};
enum Decoration : u32 {
    DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecNonWritable = 24, DecFlat = 14,
    DecNoPerspective = 13, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35,
};
enum BuiltIn : u32 { BuiltInPosition = 0, BuiltInFragCoord = 15, BuiltInFrontFacing = 17, BuiltInFragDepth = 22 };
// Operandos opcionales de las instrucciones de imagen (mascara; los valores van en este orden)
enum ImageOperand : u32 { ImgBias = 1, ImgLod = 2, ImgGrad = 4, ImgConstOffset = 8, ImgOffset = 16 };

class Builder {
public:
    u32 Id() { return m_next++; }

    // Instruccion generica en una seccion
    static void Emit(std::vector<u32>& sec, u32 op, std::initializer_list<u32> ops) {
        sec.push_back(u32((ops.size() + 1) << 16) | op);
        sec.insert(sec.end(), ops.begin(), ops.end());
    }
    static void Emit(std::vector<u32>& sec, u32 op, const std::vector<u32>& ops) {
        sec.push_back(u32((ops.size() + 1) << 16) | op);
        sec.insert(sec.end(), ops.begin(), ops.end());
    }
    // Texto como operando (terminado en 0, rellenado a palabras)
    static void PushString(std::vector<u32>& ops, const std::string& s) {
        const size_t words = s.size() / 4 + 1;
        const size_t base = ops.size();
        ops.resize(base + words, 0);
        std::memcpy(&ops[base], s.data(), s.size());
    }

    // ---- Secciones ----
    std::vector<u32> capabilities, extensions, imports, memory_model, entry_points, exec_modes,
                     debug, annotations, globals, functions;

    void Capability(u32 cap) { Emit(capabilities, OpCapability, {cap}); }
    u32 ImportGlsl() {
        if (!m_glsl) {
            m_glsl = Id();
            std::vector<u32> ops{m_glsl};
            PushString(ops, "GLSL.std.450");
            Emit(imports, OpExtInstImport, ops);
        }
        return m_glsl;
    }
    void Name(u32 id, const std::string& name) {
        std::vector<u32> ops{id};
        PushString(ops, name);
        Emit(debug, OpName, ops);
    }
    void Decorate(u32 id, u32 dec, std::initializer_list<u32> args = {}) {
        std::vector<u32> ops{id, dec};
        ops.insert(ops.end(), args.begin(), args.end());
        Emit(annotations, OpDecorate, ops);
    }
    void MemberDecorate(u32 id, u32 member, u32 dec, std::initializer_list<u32> args = {}) {
        std::vector<u32> ops{id, member, dec};
        ops.insert(ops.end(), args.begin(), args.end());
        Emit(annotations, OpMemberDecorate, ops);
    }

    // ---- Tipos (sin repetir) ----
    u32 TypeVoid() { return Cached({OpTypeVoid}, [&](u32 id) { Emit(globals, OpTypeVoid, {id}); }); }
    u32 TypeBool() { return Cached({OpTypeBool}, [&](u32 id) { Emit(globals, OpTypeBool, {id}); }); }
    u32 TypeInt(u32 bits, bool sgn) {
        return Cached({OpTypeInt, bits, u32(sgn)}, [&](u32 id) { Emit(globals, OpTypeInt, {id, bits, u32(sgn)}); });
    }
    u32 TypeFloat(u32 bits) { return Cached({OpTypeFloat, bits}, [&](u32 id) { Emit(globals, OpTypeFloat, {id, bits}); }); }
    u32 TypeVector(u32 comp, u32 n) {
        return Cached({OpTypeVector, comp, n}, [&](u32 id) { Emit(globals, OpTypeVector, {id, comp, n}); });
    }
    u32 TypeArray(u32 elem, u32 length_const) {
        return Cached({OpTypeArray, elem, length_const}, [&](u32 id) { Emit(globals, OpTypeArray, {id, elem, length_const}); });
    }
    u32 TypeRuntimeArray(u32 elem) {
        return Cached({OpTypeRuntimeArray, elem}, [&](u32 id) { Emit(globals, OpTypeRuntimeArray, {id, elem}); });
    }
    u32 TypeStruct(const std::vector<u32>& members) {   // siempre nuevo (las decoraciones son por struct)
        const u32 id = Id();
        std::vector<u32> ops{id};
        ops.insert(ops.end(), members.begin(), members.end());
        Emit(globals, OpTypeStruct, ops);
        return id;
    }
    u32 TypePointer(u32 storage, u32 type) {
        return Cached({OpTypePointer, storage, type}, [&](u32 id) { Emit(globals, OpTypePointer, {id, storage, type}); });
    }
    u32 TypeFunction(u32 ret) {
        return Cached({OpTypeFunction, ret}, [&](u32 id) { Emit(globals, OpTypeFunction, {id, ret}); });
    }
    u32 TypeImage(u32 sampled_type, u32 dim, u32 depth, u32 arrayed, u32 ms, u32 sampled, u32 format) {
        return Cached({OpTypeImage, sampled_type, dim, depth, arrayed, ms, sampled, format}, [&](u32 id) {
            Emit(globals, OpTypeImage, {id, sampled_type, dim, depth, arrayed, ms, sampled, format});
        });
    }
    u32 TypeSampledImage(u32 image) {
        return Cached({OpTypeSampledImage, image}, [&](u32 id) { Emit(globals, OpTypeSampledImage, {id, image}); });
    }

    // ---- Constantes (sin repetir) ----
    u32 ConstU32(u32 v) {
        const u32 t = TypeInt(32, false);
        return Cached({OpConstant, t, v}, [&](u32 id) { Emit(globals, OpConstant, {t, id, v}); });
    }
    u32 ConstF32(float f) {
        u32 v; std::memcpy(&v, &f, 4);
        const u32 t = TypeFloat(32);
        return Cached({OpConstant, t, v}, [&](u32 id) { Emit(globals, OpConstant, {t, id, v}); });
    }
    u32 ConstU64(u64 v) {
        const u32 t = TypeInt(64, false);
        return Cached({OpConstant, t, u32(v), u32(v >> 32)}, [&](u32 id) { Emit(globals, OpConstant, {t, id, u32(v), u32(v >> 32)}); });
    }
    u32 ConstBool(bool v) {
        const u32 t = TypeBool();
        return Cached({v ? u32(OpConstantTrue) : u32(OpConstantFalse), t}, [&](u32 id) {
            Emit(globals, v ? OpConstantTrue : OpConstantFalse, {t, id});
        });
    }
    u32 ConstComposite(u32 type, const std::vector<u32>& parts) {
        std::vector<u32> key{OpConstantComposite, type};
        key.insert(key.end(), parts.begin(), parts.end());
        return Cached(key, [&](u32 id) {
            std::vector<u32> ops{type, id};
            ops.insert(ops.end(), parts.begin(), parts.end());
            Emit(globals, OpConstantComposite, ops);
        });
    }

    // Variable global (no de funcion)
    u32 GlobalVariable(u32 ptr_type, u32 storage) {
        const u32 id = Id();
        Emit(globals, OpVariable, {ptr_type, id, storage});
        return id;
    }

    // Modulo completo
    std::vector<u32> Assemble() const {
        std::vector<u32> out{0x07230203u, 0x00010300u /* SPIR-V 1.3 */, 0x4E580000u /* "NX" */, m_next, 0};
        for (const auto* s : {&capabilities, &extensions, &imports, &memory_model, &entry_points, &exec_modes,
                              &debug, &annotations, &globals, &functions})
            out.insert(out.end(), s->begin(), s->end());
        return out;
    }

private:
    template <typename F> u32 Cached(const std::vector<u32>& key, F&& make) {
        auto it = m_cache.find(key);
        if (it != m_cache.end()) return it->second;
        const u32 id = Id();
        make(id);
        m_cache.emplace(key, id);
        return id;
    }
    u32 m_next = 1;
    u32 m_glsl = 0;
    std::map<std::vector<u32>, u32> m_cache;
};

} // namespace NeXo2::GPU::Spv
