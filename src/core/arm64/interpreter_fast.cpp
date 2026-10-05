// Cache de instrucciones decodificadas: el decodificador y las funciones "rapidas".
//
// Decode() mira los bits de una instruccion UNA vez y rellena un DecodedInstr con:
//   - fn: la funcion que la ejecuta
//   - los operandos ya extraidos (registros, inmediatos, mascaras, destinos de salto)
// Despues, cada vez que el PC pasa por esa direccion, solo se llama a fn.
//
// Solo las instrucciones mas frecuentes tienen funcion propia (ADD, SUB, MOV, CMP,
// AND/ORR, LSL/LSR, CSEL, MUL, saltos, LDR/STR, LDP/STP...). Todas las demas usan
// Generic(), que llama al decodificador normal (Execute) como siempre.
//
// REGLA: una funcion rapida tiene que hacer EXACTAMENTE lo mismo que el codigo normal.
// Por eso Decode() solo elige una funcion rapida cuando el codigo normal aceptaria la
// instruccion; si no, usa Generic() y el codigo normal decide (y da el mismo error).
// tests/decode_cache_tests.cpp lo comprueba ejecutando miles de instrucciones al azar
// por los dos caminos.
#include "interpreter.hpp"
#include "common/bit_utils.hpp"

namespace NeXo2::Core {

using namespace NeXo2::Common;

struct FastOps {
    using D = DecodedInstr;

    static unsigned DataSize(const D& d) { return d.sf ? 64 : 32; }

    // ------------------------------------------------------------------
    // Cualquier otra instruccion: decodificador normal
    // ------------------------------------------------------------------
    static bool Generic(Interpreter& it, const D& d) { return it.Execute(d.raw); }

    // ------------------------------------------------------------------
    // Procesado de datos con inmediato
    // ------------------------------------------------------------------
    // ADD/ADDS/SUB/SUBS #imm (CMP, CMN, MOV x0, sp). imm ya desplazado.
    template <bool SUB, bool FLAGS>
    static bool AddSubImm(Interpreter& it, const D& d) {
        const bool sf = d.sf;
        const u64 a = it.XorSP(d.rn, sf);
        const u64 r = SUB ? it.AddWithCarry(a, ~d.imm, true, sf, FLAGS)
                          : it.AddWithCarry(a, d.imm, false, sf, FLAGS);
        if (FLAGS) it.SetX(d.rd, r, sf);
        else       it.SetXorSP(d.rd, r, sf);
        return true;
    }

    // AND/ORR/EOR/ANDS #mascara (la mascara ya calculada en imm)
    template <u32 OPC>
    static bool LogicImm(Interpreter& it, const D& d) {
        const u64 a = it.X(d.rn, d.sf);
        const u64 r = (OPC == 1) ? (a | d.imm) : (OPC == 2) ? (a ^ d.imm) : (a & d.imm);
        if (OPC == 3) { it.SetLogicFlags(r, d.sf); it.SetX(d.rd, r, d.sf); }
        else          it.SetXorSP(d.rd, r, d.sf);
        return true;
    }

    // MOVN / MOVZ / MOVK (imm = valor ya desplazado, a = desplazamiento)
    static bool MovN(Interpreter& it, const D& d) { it.SetX(d.rd, ~d.imm, d.sf); return true; }
    static bool MovZ(Interpreter& it, const D& d) { it.SetX(d.rd, d.imm, d.sf); return true; }
    static bool MovK(Interpreter& it, const D& d) {
        u64 v = it.X(d.rd, d.sf);
        v &= ~(0xFFFFULL << d.a);
        it.SetX(d.rd, v | d.imm, d.sf);
        return true;
    }

    // SBFM/BFM/UBFM en general (imm = wmask, imm2 = tmask, a = immr, b = imms, c = opc)
    static bool Bitfield(Interpreter& it, const D& d) {
        const unsigned ds = DataSize(d);
        const u64 src = it.X(d.rn, d.sf);
        const u64 rotated = RotateRight(src, d.a, ds);
        u64 result;
        if (d.c == 0b01) {                                   // BFM
            const u64 dst = it.X(d.rd, d.sf);
            const u64 bot = (dst & ~d.imm) | (rotated & d.imm);
            result = (dst & ~d.imm2) | (bot & d.imm2);
        } else {                                             // SBFM / UBFM
            const u64 bot = rotated & d.imm;
            const u64 top = (d.c == 0b00 && ((src >> d.b) & 1)) ? Ones(ds) : 0;
            result = (top & ~d.imm2) | (bot & d.imm2);
        }
        it.SetX(d.rd, result & Ones(ds), d.sf);
        return true;
    }
    // Los alias mas comunes de UBFM/SBFM, sin mascaras: LSR #, LSL #, ASR #
    static bool LsrImm(Interpreter& it, const D& d) { it.SetX(d.rd, it.X(d.rn, d.sf) >> d.a, d.sf); return true; }
    static bool LslImm(Interpreter& it, const D& d) {
        it.SetX(d.rd, (it.X(d.rn, d.sf) << d.a) & Ones(DataSize(d)), d.sf);
        return true;
    }
    static bool AsrImm(Interpreter& it, const D& d) {
        const unsigned ds = DataSize(d);
        it.SetX(d.rd, u64(SignExtend(it.X(d.rn, d.sf), ds) >> d.a) & Ones(ds), d.sf);
        return true;
    }

    // ------------------------------------------------------------------
    // Procesado de datos con registros
    // ------------------------------------------------------------------
    // AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS con registro desplazado
    // (a = tipo de desplazamiento, b = cantidad, c = 1 si es la version negada)
    template <u32 OPC, bool SHIFTED>
    static bool LogicReg(Interpreter& it, const D& d) {
        u64 b = SHIFTED ? it.ShiftReg(it.X(d.rm, d.sf), d.a, d.b, d.sf) : it.X(d.rm, d.sf);
        if (d.c) b = ~b & Ones(DataSize(d));
        const u64 a = it.X(d.rn, d.sf);
        const u64 r = (OPC == 1) ? (a | b) : (OPC == 2) ? (a ^ b) : (a & b);
        if (OPC == 3) it.SetLogicFlags(r, d.sf);
        it.SetX(d.rd, r, d.sf);
        return true;
    }
    // MOV x0, x1 (= ORR x0, xzr, x1 sin desplazamiento)
    static bool MovReg(Interpreter& it, const D& d) { it.SetX(d.rd, it.X(d.rm, d.sf), d.sf); return true; }

    // ADD/ADDS/SUB/SUBS con registro desplazado (CMP x0, x1)
    template <bool SUB, bool FLAGS, bool SHIFTED>
    static bool AddSubReg(Interpreter& it, const D& d) {
        const bool sf = d.sf;
        const u64 a = it.X(d.rn, sf);
        const u64 b = SHIFTED ? it.ShiftReg(it.X(d.rm, sf), d.a, d.b, sf) : it.X(d.rm, sf);
        const u64 r = SUB ? it.AddWithCarry(a, ~b, true, sf, FLAGS)
                          : it.AddWithCarry(a, b, false, sf, FLAGS);
        it.SetX(d.rd, r, sf);
        return true;
    }

    // CSEL / CSINC / CSINV / CSNEG (a = condicion)
    template <u32 VARIANT>
    static bool CondSelect(Interpreter& it, const D& d) {
        u64 r;
        if (it.ConditionHolds(d.a)) {
            r = it.X(d.rn, d.sf);
        } else {
            const u64 b = it.X(d.rm, d.sf);
            r = (VARIANT == 0) ? b : (VARIANT == 1) ? b + 1 : (VARIANT == 2) ? ~b : 0 - b;
        }
        it.SetX(d.rd, r & Ones(DataSize(d)), d.sf);
        return true;
    }

    // LSLV / LSRV / ASRV / RORV (TYPE = tipo de desplazamiento)
    template <u32 TYPE>
    static bool ShiftVar(Interpreter& it, const D& d) {
        const unsigned ds = DataSize(d);
        const u64 a = it.X(d.rn, d.sf), b = it.X(d.rm, d.sf);
        it.SetX(d.rd, it.ShiftReg(a, TYPE, unsigned(b & (ds - 1)), d.sf) & Ones(ds), d.sf);
        return true;
    }

    // MADD / MSUB (MUL, MNEG) (a = 1 si es resta)
    static bool MulAdd(Interpreter& it, const D& d) {
        const u64 prod = it.X(d.rn, d.sf) * it.X(d.rm, d.sf);
        const u64 acc = it.X(d.ra, d.sf);
        it.SetX(d.rd, (d.a ? acc - prod : acc + prod) & Ones(DataSize(d)), d.sf);
        return true;
    }

    // ------------------------------------------------------------------
    // Saltos (imm = destino ya calculado, imm2 = direccion de retorno)
    // ------------------------------------------------------------------
    static bool Branch(Interpreter& it, const D& d) { it.m_nextPc = d.imm; return true; }
    static bool BranchLink(Interpreter& it, const D& d) {
        it.m_state.x[30] = d.imm2;
        it.m_nextPc = d.imm;
        return true;
    }
    static bool BranchCond(Interpreter& it, const D& d) {
        if (it.ConditionHolds(d.a)) it.m_nextPc = d.imm;
        return true;
    }
    template <bool NZ>
    static bool CompareBranch(Interpreter& it, const D& d) {      // CBZ / CBNZ
        const bool is_zero = it.X(d.rd, d.sf) == 0;
        if (is_zero != NZ) it.m_nextPc = d.imm;
        return true;
    }
    template <bool NZ>
    static bool TestBranch(Interpreter& it, const D& d) {         // TBZ / TBNZ (a = bit)
        const bool bit_set = (it.X(d.rd) >> d.a) & 1;
        if (bit_set == NZ) it.m_nextPc = d.imm;
        return true;
    }
    static bool BranchReg(Interpreter& it, const D& d) { it.m_nextPc = it.X(d.rn); return true; }   // BR / RET
    static bool BranchLinkReg(Interpreter& it, const D& d) {                                       // BLR
        const u64 target = it.X(d.rn);
        it.m_state.x[30] = d.imm2;
        it.m_nextPc = target;
        return true;
    }

    // ------------------------------------------------------------------
    // Lecturas y escrituras de un registro
    //   rd = Rt, ra = bytes, a = tipo de acceso, b = opcion de extension, c = desplazamiento
    // ------------------------------------------------------------------
    // Modo de direccionamiento. Con registro indice hay 4 variantes segun la extension:
    //   [Xn, Wm, UXTW]  [Xn, Xm, LSL]  [Xn, Wm, SXTW]  [Xn, Xm, SXTX]
    enum AddrMode { UIMM, REG_UXTW, REG_LSL, REG_SXTW, REG_SXTX, UNSCALED, POST, PRE };
    enum Access : u8 { STORE, LOAD_U, LOAD_S64, LOAD_S32, PREFETCH };

    template <int MODE>
    static u64 Address(Interpreter& it, const D& d, u64 base) {
        if constexpr (MODE == REG_UXTW) return base + ((it.X(d.rm) & 0xFFFFFFFFu) << d.c);
        else if constexpr (MODE == REG_LSL)  return base + (it.X(d.rm) << d.c);
        else if constexpr (MODE == REG_SXTW) return base + (u64(SignExtend(it.X(d.rm), 32)) << d.c);
        else if constexpr (MODE == REG_SXTX) return base + (it.X(d.rm) << d.c);
        else if constexpr (MODE == POST)     return base;
        else                                 return base + d.imm;       // UIMM, UNSCALED, PRE
    }

    // Caso comun: LDR/STR sin signo con tamano fijo (todo resuelto al compilar)
    template <int MODE, bool STORE_OP, unsigned BYTES>
    static bool LoadStoreFixed(Interpreter& it, const D& d) {
        const u64 base = it.XorSP(d.rn);
        const u64 addr = Address<MODE>(it, d, base);
        if constexpr (STORE_OP) it.WriteMemory(addr, it.X(d.rd), BYTES);
        else                    it.SetX(d.rd, it.ReadMemory(addr, BYTES));
        if constexpr (MODE == POST || MODE == PRE) it.SetXorSP(d.rn, base + d.imm);
        return true;
    }

    // Resto: cargas con signo y PRFM (rd = Rt, ra = bytes, a = tipo de acceso)
    template <int MODE>
    static bool LoadStoreAny(Interpreter& it, const D& d) {
        const u64 base = it.XorSP(d.rn);
        const u64 addr = Address<MODE>(it, d, base);
        const unsigned bytes = d.ra;
        switch (d.a) {
            case STORE:    it.WriteMemory(addr, it.X(d.rd), bytes); break;
            case LOAD_U:   it.SetX(d.rd, it.ReadMemory(addr, bytes)); break;
            case LOAD_S64: it.SetX(d.rd, u64(SignExtend(it.ReadMemory(addr, bytes), bytes * 8))); break;
            case LOAD_S32: it.SetX(d.rd, u64(SignExtend(it.ReadMemory(addr, bytes), bytes * 8)) & 0xFFFFFFFFu, false); break;
            default: break;                                       // PRFM: no hace nada
        }
        if constexpr (MODE == POST || MODE == PRE) it.SetXorSP(d.rn, base + d.imm);
        return true;
    }

    template <int MODE>
    static D::Handler PickLoadStore(u8 access, unsigned bytes) {
        if (access == STORE || access == LOAD_U) {
            const bool st = (access == STORE);
            switch (bytes) {
                case 1: return st ? &LoadStoreFixed<MODE, true, 1> : &LoadStoreFixed<MODE, false, 1>;
                case 2: return st ? &LoadStoreFixed<MODE, true, 2> : &LoadStoreFixed<MODE, false, 2>;
                case 4: return st ? &LoadStoreFixed<MODE, true, 4> : &LoadStoreFixed<MODE, false, 4>;
                default: return st ? &LoadStoreFixed<MODE, true, 8> : &LoadStoreFixed<MODE, false, 8>;
            }
        }
        return &LoadStoreAny<MODE>;
    }

    // LDP / STP / LDPSW (rd = Rt, ra = Rt2, imm = offset, b = bytes, a = tipo, c: bit0 load, bit1 LDPSW)
    static bool LoadStorePair(Interpreter& it, const D& d) {
        const unsigned bytes = d.b;
        const u64 base = it.XorSP(d.rn);
        const u64 addr = (d.a == 0b01) ? base : base + d.imm;
        if (d.c & 1) {
            u64 x = it.ReadMemory(addr, bytes);
            u64 y = it.ReadMemory(addr + bytes, bytes);
            if (d.c & 2) { x = u64(SignExtend(x, 32)); y = u64(SignExtend(y, 32)); }
            it.SetX(d.rd, x);
            it.SetX(d.ra, y);
        } else {
            it.WriteMemory(addr, it.X(d.rd), bytes);
            it.WriteMemory(addr + bytes, it.X(d.ra), bytes);
        }
        if (d.a == 0b01 || d.a == 0b11) it.SetXorSP(d.rn, base + d.imm);
        return true;
    }

    // ==================================================================
    //  Decodificador
    // ==================================================================
    static void DecodeDataProcImm(u32 raw, u64 pc, D& d);
    static void DecodeDataProcReg(u32 raw, D& d);
    static void DecodeBranch(u32 raw, u64 pc, D& d);
    static void DecodeLoadStore(u32 raw, D& d);
};

void FastOps::DecodeDataProcImm(u32 raw, u64 /*pc*/, D& d) {
    const u32 op = Bits(raw, 23, 3);
    const bool sf = Bit(raw, 31);
    const unsigned ds = sf ? 64 : 32;
    d.sf = sf;
    d.rd = u8(Bits(raw, 0, 5));
    d.rn = u8(Bits(raw, 5, 5));

    switch (op) {
    case 0b010: {                                            // ADD/SUB #imm
        d.imm = Bits(raw, 10, 12);
        if (Bit(raw, 22)) d.imm <<= 12;
        static constexpr D::Handler table[4] = {
            &AddSubImm<false, false>, &AddSubImm<false, true>, &AddSubImm<true, false>, &AddSubImm<true, true>};
        d.fn = table[Bits(raw, 29, 2)];   // bit 30 = resta, bit 29 = flags
        return;
    }
    case 0b100: {                                            // AND/ORR/EOR/ANDS #mascara
        const u32 n = Bit(raw, 22);
        if (!sf && n) return;
        u64 wmask, tmask;
        if (!DecodeBitMasks(n, Bits(raw, 10, 6), Bits(raw, 16, 6), true, ds, wmask, tmask)) return;
        d.imm = wmask;
        static constexpr D::Handler table[4] = {&LogicImm<0>, &LogicImm<1>, &LogicImm<2>, &LogicImm<3>};
        d.fn = table[Bits(raw, 29, 2)];
        return;
    }
    case 0b101: {                                            // MOVN / MOVZ / MOVK
        const u32 opc = Bits(raw, 29, 2);
        const u32 hw = Bits(raw, 21, 2);
        if ((!sf && hw >= 2) || opc == 0b01) return;
        d.a = u8(hw * 16);
        d.imm = u64(Bits(raw, 5, 16)) << d.a;
        d.fn = (opc == 0b00) ? &MovN : (opc == 0b10) ? &MovZ : &MovK;
        return;
    }
    case 0b110: {                                            // SBFM / BFM / UBFM
        const u32 opc = Bits(raw, 29, 2);
        const u32 n = Bit(raw, 22);
        const u32 immr = Bits(raw, 16, 6);
        const u32 imms = Bits(raw, 10, 6);
        if (opc == 0b11 || n != (sf ? 1u : 0u)) return;
        if (!sf && (immr >= 32 || imms >= 32)) return;
        u64 wmask, tmask;
        if (!DecodeBitMasks(n, imms, immr, false, ds, wmask, tmask)) return;
        d.imm = wmask; d.imm2 = tmask;
        d.a = u8(immr); d.b = u8(imms); d.c = u8(opc);
        d.fn = &Bitfield;
        // Alias frecuentes
        if (opc == 0b10 && imms == ds - 1)             { d.fn = &LsrImm; }                       // LSR #immr
        else if (opc == 0b00 && imms == ds - 1)        { d.fn = &AsrImm; }                       // ASR #immr
        else if (opc == 0b10 && imms + 1 == immr)      { d.a = u8(ds - 1 - imms); d.fn = &LslImm; } // LSL #n
        return;
    }
    default:
        return;                                              // ADR/ADRP, EXTR: Generic
    }
}

void FastOps::DecodeDataProcReg(u32 raw, D& d) {
    const bool sf = Bit(raw, 31);
    const u32 op1 = Bit(raw, 28);
    const u32 op2 = Bits(raw, 21, 4);
    d.sf = sf;
    d.rd = u8(Bits(raw, 0, 5));
    d.rn = u8(Bits(raw, 5, 5));
    d.rm = u8(Bits(raw, 16, 5));

    if (op1 == 0) {
        const u32 shift = Bits(raw, 22, 2);
        const u32 imm6 = Bits(raw, 10, 6);
        if ((op2 & 0b1000) == 0) {                           // logico con registro desplazado
            if (!sf && imm6 >= 32) return;
            d.a = u8(shift); d.b = u8(imm6); d.c = u8(Bit(raw, 21));
            const u32 opc = Bits(raw, 29, 2);
            static constexpr D::Handler table[2][4] = {
                {&LogicReg<0, false>, &LogicReg<1, false>, &LogicReg<2, false>, &LogicReg<3, false>},
                {&LogicReg<0, true>,  &LogicReg<1, true>,  &LogicReg<2, true>,  &LogicReg<3, true>}};
            d.fn = table[imm6 != 0][opc];   // sin desplazamiento (lo normal) no hace falta ShiftReg
            if (opc == 0b01 && d.rn == 31 && imm6 == 0 && !d.c) d.fn = &MovReg; // MOV xd, xm
            return;
        }
        if ((op2 & 0b1001) == 0b1000) {                      // ADD/SUB con registro desplazado
            if (shift == 3 || (!sf && imm6 >= 32)) return;
            d.a = u8(shift); d.b = u8(imm6);
            static constexpr D::Handler table[2][4] = {
                {&AddSubReg<false, false, false>, &AddSubReg<false, true, false>,
                 &AddSubReg<true, false, false>,  &AddSubReg<true, true, false>},
                {&AddSubReg<false, false, true>,  &AddSubReg<false, true, true>,
                 &AddSubReg<true, false, true>,   &AddSubReg<true, true, true>}};
            d.fn = table[imm6 != 0][Bits(raw, 29, 2)];
            return;
        }
        return;                                              // registro extendido: Generic
    }

    if (op2 == 0b0100) {                                     // CSEL / CSINC / CSINV / CSNEG
        const u32 op2b = Bits(raw, 10, 2);
        if (Bit(raw, 29) || op2b > 1) return;
        d.a = u8(Bits(raw, 12, 4));
        static constexpr D::Handler table[4] = {&CondSelect<0>, &CondSelect<1>, &CondSelect<2>, &CondSelect<3>};
        d.fn = table[(Bit(raw, 30) << 1) | op2b];
        return;
    }
    if (op2 == 0b0110) {                                     // LSLV / LSRV / ASRV / RORV
        const u32 opcode = Bits(raw, 10, 6);
        if (Bit(raw, 29) || Bit(raw, 30) || (opcode & 0b111100) != 0b001000) return;
        static constexpr D::Handler table[4] = {&ShiftVar<0>, &ShiftVar<1>, &ShiftVar<2>, &ShiftVar<3>};
        d.fn = table[opcode & 3];
        return;
    }
    if (op2 & 0b1000) {                                      // MADD / MSUB
        if (Bits(raw, 29, 2) != 0 || Bits(raw, 21, 3) != 0) return;
        d.ra = u8(Bits(raw, 10, 5));
        d.a = u8(Bit(raw, 15));
        d.fn = &MulAdd;
        return;
    }
}

void FastOps::DecodeBranch(u32 raw, u64 pc, D& d) {
    if (Bits(raw, 26, 5) == 0b00101) {                       // B / BL
        d.imm = pc + u64(SignExtend(Bits(raw, 0, 26), 26) * 4);
        d.imm2 = pc + 4;
        d.fn = Bit(raw, 31) ? &BranchLink : &Branch;
        return;
    }
    if (Bits(raw, 25, 6) == 0b011010) {                      // CBZ / CBNZ
        d.sf = Bit(raw, 31);
        d.rd = u8(Bits(raw, 0, 5));
        d.imm = pc + u64(SignExtend(Bits(raw, 5, 19), 19) * 4);
        d.fn = Bit(raw, 24) ? &CompareBranch<true> : &CompareBranch<false>;
        return;
    }
    if (Bits(raw, 25, 6) == 0b011011) {                      // TBZ / TBNZ
        d.rd = u8(Bits(raw, 0, 5));
        d.a = u8((Bit(raw, 31) << 5) | Bits(raw, 19, 5));
        d.imm = pc + u64(SignExtend(Bits(raw, 5, 14), 14) * 4);
        d.fn = Bit(raw, 24) ? &TestBranch<true> : &TestBranch<false>;
        return;
    }
    if (Bits(raw, 24, 8) == 0b01010100 && Bit(raw, 4) == 0) { // B.cond
        d.a = u8(Bits(raw, 0, 4));
        d.imm = pc + u64(SignExtend(Bits(raw, 5, 19), 19) * 4);
        d.fn = &BranchCond;
        return;
    }
    d.rn = u8(Bits(raw, 5, 5));
    switch (raw & 0xFFFFFC1Fu) {
        case 0xD61F0000u:                                    // BR
        case 0xD65F0000u: d.fn = &BranchReg; return;         // RET
        case 0xD63F0000u: d.imm2 = pc + 4; d.fn = &BranchLinkReg; return;  // BLR
        default: return;                                     // SVC, MRS, NOP...: Generic
    }
}

void FastOps::DecodeLoadStore(u32 raw, D& d) {
    if (Bit(raw, 26)) return;                                // SIMD: Generic
    if (Bits(raw, 24, 6) == 0b001000) return;                // exclusivas / atomicas: Generic
    const u32 group = Bits(raw, 28, 2);
    d.rd = u8(Bits(raw, 0, 5));
    d.rn = u8(Bits(raw, 5, 5));

    if (group == 0b10) {                                     // LDP / STP / LDPSW
        const u32 opc = Bits(raw, 30, 2);
        const bool load = Bit(raw, 22);
        if (opc == 0b11 || (opc == 0b01 && !load)) return;
        const unsigned bytes = (opc == 0b10) ? 8 : 4;
        d.ra = u8(Bits(raw, 10, 5));
        d.b = u8(bytes);
        d.a = u8(Bits(raw, 23, 2));
        d.c = u8((load ? 1 : 0) | (opc == 0b01 ? 2 : 0));
        d.imm = u64(SignExtend(Bits(raw, 15, 7), 7) * s64(bytes));
        d.fn = &LoadStorePair;
        return;
    }
    if (group != 0b11) return;                               // LDR literal: Generic

    const u32 size = Bits(raw, 30, 2);
    const u32 opc = Bits(raw, 22, 2);
    const unsigned bytes = 1u << size;
    d.ra = u8(bytes);

    // Tipo de acceso (igual que ExecLoadStore)
    if (opc == 0b00)                    d.a = STORE;
    else if (opc == 0b10 && size == 3)  d.a = PREFETCH;
    else if (opc == 0b11 && size >= 2)  return;              // invalida: Generic da el error
    else if (opc == 0b01)               d.a = LOAD_U;
    else if (opc == 0b10)               d.a = LOAD_S64;
    else                                d.a = LOAD_S32;

    if (Bit(raw, 24)) {                                      // [Xn, #imm12 * tamano]
        d.imm = u64(Bits(raw, 10, 12)) << size;
        d.fn = PickLoadStore<UIMM>(d.a, bytes);
        return;
    }
    if (Bit(raw, 21) == 0) {                                 // imm9: LDUR, post-indice, pre-indice
        const u32 mode = Bits(raw, 10, 2);
        const bool writeback = (mode == 0b01 || mode == 0b11);
        if (writeback && opc == 0b10 && size == 3) return;
        d.imm = u64(SignExtend(Bits(raw, 12, 9), 9));
        d.fn = (mode == 0b01) ? PickLoadStore<POST>(d.a, bytes)
             : (mode == 0b11) ? PickLoadStore<PRE>(d.a, bytes)
                              : PickLoadStore<UNSCALED>(d.a, bytes);
        return;
    }
    if (Bits(raw, 10, 2) == 0b10) {                          // [Xn, Xm{, extension}]
        const u32 option = Bits(raw, 13, 3);
        if ((option & 0b010) == 0) return;
        d.rm = u8(Bits(raw, 16, 5));
        d.c = u8(Bit(raw, 12) ? size : 0);
        switch (option) {          // solo quedan 010, 011, 110, 111
            case 0b010: d.fn = PickLoadStore<REG_UXTW>(d.a, bytes); break;
            case 0b011: d.fn = PickLoadStore<REG_LSL>(d.a, bytes);  break;
            case 0b110: d.fn = PickLoadStore<REG_SXTW>(d.a, bytes); break;
            default:    d.fn = PickLoadStore<REG_SXTX>(d.a, bytes); break;
        }
        return;
    }
    // Atomicos LSE: Generic
}

void Interpreter::Decode(u32 raw, u64 pc, DecodedInstr& d) {
    d = DecodedInstr{};
    d.raw = raw;
    d.fn = &FastOps::Generic;
    const u32 op0 = Bits(raw, 25, 4);        // mismos grupos que Execute()
    if ((op0 & 0b1110) == 0b1000)      FastOps::DecodeDataProcImm(raw, pc, d);
    else if ((op0 & 0b1110) == 0b1010) FastOps::DecodeBranch(raw, pc, d);
    else if ((op0 & 0b0101) == 0b0100) FastOps::DecodeLoadStore(raw, d);
    else if ((op0 & 0b0111) == 0b0101) FastOps::DecodeDataProcReg(raw, d);
}

} // namespace NeXo2::Core
