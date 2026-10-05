// GENERADO por tools/asm2cpp.py a partir de tests/programs/. NO EDITAR A MANO.
#pragma once
#include <cstdint>

namespace NeXo2::Tests::Programs {

// add_sub_sp.S
alignas(16) inline const uint32_t add_sub_sp[] = {
    0x910003E0u, // 0000: mov x0, sp
    0xD10083FFu, // 0004: sub sp, sp, #0x20
    0x910003E1u, // 0008: mov x1, sp
    0x91400422u, // 000C: add x2, x1, #0x1, lsl #12 // =0x1000
    0x12800003u, // 0010: mov w3, #-0x1 // =-1
    0x11000863u, // 0014: add w3, w3, #0x2
    0x910083FFu, // 0018: add sp, sp, #0x20
    0xD4200000u, // 001C: brk #0
};

// adr.S
alignas(16) inline const uint32_t adr[] = {
    0x10000060u, // 0000: adr x0, 0xc <label>
    0x90000001u, // 0004: adrp x1, 0x0 <.text>
    0xD503201Fu, // 0008: nop
    0xD4200000u, // 000C: brk #0
};

// atomics.S
alignas(16) inline const uint32_t atomics[] = {
    0xD2B20001u, // 0000: mov x1, #0x90000000 // =2415919104
    0xD2800142u, // 0004: mov x2, #0xa // =10
    0xF9000022u, // 0008: str x2, [x1]
    0xC85FFC23u, // 000C: ldaxr x3, [x1]
    0x91001463u, // 0010: add x3, x3, #0x5
    0xC804FC23u, // 0014: stlxr w4, x3, [x1]
    0x35FFFFA4u, // 0018: cbnz w4, 0xc <.text+0xc>
    0xF9400025u, // 001C: ldr x5, [x1]
    0xD2800066u, // 0020: mov x6, #0x3 // =3
    0xF8260027u, // 0024: ldadd x6, x7, [x1]
    0xD2800C88u, // 0028: mov x8, #0x64 // =100
    0xF8288029u, // 002C: swp x8, x9, [x1]
    0xD2800C8Au, // 0030: mov x10, #0x64 // =100
    0xD280190Bu, // 0034: mov x11, #0xc8 // =200
    0xC8AA7C2Bu, // 0038: cas x10, x11, [x1]
    0xD28000ACu, // 003C: mov x12, #0x5 // =5
    0xC8AC7C2Bu, // 0040: cas x12, x11, [x1]
    0xC89FFC22u, // 0044: stlr x2, [x1]
    0xC8DFFC2Du, // 0048: ldar x13, [x1]
    0xD4200000u, // 004C: brk #0
};

// bitfield.S
alignas(16) inline const uint32_t bitfield[] = {
    0xD297DDE0u, // 0000: mov x0, #0xbeef // =48879
    0xF2BBD5A0u, // 0004: movk x0, #0xdead, lsl #16
    0xF2F00000u, // 0008: movk x0, #0x8000, lsl #48
    0x92401C01u, // 000C: and x1, x0, #0xff
    0xB201F3E2u, // 0010: mov x2, #-0x5555555555555556 // =-6148914691236517206
    0xD2103C03u, // 0014: eor x3, x0, #0xffff0000ffff0000
    0xD37CEC04u, // 0018: lsl x4, x0, #4
    0xD37CFC05u, // 001C: lsr x5, x0, #60
    0x937CFC06u, // 0020: asr x6, x0, #60
    0xD3483C07u, // 0024: ubfx x7, x0, #8, #8
    0x934C3C08u, // 0028: sbfx x8, x0, #12, #4
    0xD2800009u, // 002C: mov x9, #0x0 // =0
    0xB3701C09u, // 0030: bfi x9, x0, #16, #8
    0x93407C0Au, // 0034: sxtw x10, w0
    0x53001C0Bu, // 0038: uxtb w11, w0
    0x93403C0Cu, // 003C: sxth x12, w0
    0x93C0100Du, // 0040: ror x13, x0, #0x4
    0xF241001Fu, // 0044: tst x0, #0x8000000000000000
    0x9A9F07EEu, // 0048: cset x14, ne
    0xD4200000u, // 004C: brk #0
};

// branches.S
alignas(16) inline const uint32_t branches[] = {
    0xD2800000u, // 0000: mov x0, #0x0 // =0
    0xD2800001u, // 0004: mov x1, #0x0 // =0
    0xB4000040u, // 0008: cbz x0, 0x10 <.text+0x10>
    0xD2800C61u, // 000C: mov x1, #0x63 // =99
    0xD2800082u, // 0010: mov x2, #0x4 // =4
    0x37100042u, // 0014: tbnz w2, #0x2, 0x1c <.text+0x1c>
    0xD2800C41u, // 0018: mov x1, #0x62 // =98
    0x36000042u, // 001C: tbz w2, #0x0, 0x24 <.text+0x24>
    0xD2800C21u, // 0020: mov x1, #0x61 // =97
    0xB5000042u, // 0024: cbnz x2, 0x2c <.text+0x2c>
    0xD2800C01u, // 0028: mov x1, #0x60 // =96
    0x14000002u, // 002C: b 0x34 <.text+0x34>
    0xD2800BE1u, // 0030: mov x1, #0x5f // =95
    0xD2800023u, // 0034: mov x3, #0x1 // =1
    0xD4200000u, // 0038: brk #0
};

// call_ret.S
alignas(16) inline const uint32_t call_ret[] = {
    0xD28000C0u, // 0000: mov x0, #0x6 // =6
    0x94000007u, // 0004: bl 0x20 <square>
    0xAA0003F3u, // 0008: mov x19, x0
    0xD28000E0u, // 000C: mov x0, #0x7 // =7
    0x10000089u, // 0010: adr x9, 0x20 <square>
    0xD63F0120u, // 0014: blr x9
    0xAA0003F4u, // 0018: mov x20, x0
    0xD4200000u, // 001C: brk #0
    0xD503233Fu, // 0020: paciasp
    0xA9BF7BFDu, // 0024: stp x29, x30, [sp, #-0x10]!
    0x910003FDu, // 0028: mov x29, sp
    0x9B007C00u, // 002C: mul x0, x0, x0
    0xA8C17BFDu, // 0030: ldp x29, x30, [sp], #0x10
    0xD50323BFu, // 0034: autiasp
    0xD65F03C0u, // 0038: ret
};

// dp_reg.S
alignas(16) inline const uint32_t dp_reg[] = {
    0xD2800C80u, // 0000: mov x0, #0x64 // =100
    0xD28000E1u, // 0004: mov x1, #0x7 // =7
    0x9AC10802u, // 0008: udiv x2, x0, x1
    0x9B018043u, // 000C: msub x3, x2, x1, x0
    0x92800C64u, // 0010: mov x4, #-0x64 // =-100
    0x9AC10C85u, // 0014: sdiv x5, x4, x1
    0x9ADF0806u, // 0018: udiv x6, x0, xzr
    0x9B017C07u, // 001C: mul x7, x0, x1
    0x92800008u, // 0020: mov x8, #-0x1 // =-1
    0x9BC87D09u, // 0024: umulh x9, x8, x8
    0x9B487D0Au, // 0028: smulh x10, x8, x8
    0xDAC0102Bu, // 002C: clz x11, x1
    0xD280204Cu, // 0030: mov x12, #0x102 // =258
    0xF2A0608Cu, // 0034: movk x12, #0x304, lsl #16
    0x5AC0098Du, // 0038: rev w13, w12
    0xEB01001Fu, // 003C: cmp x0, x1
    0x9A81C00Eu, // 0040: csel x14, x0, x1, gt
    0x9A81B40Fu, // 0044: csinc x15, x0, x1, lt
    0xDA819430u, // 0048: cneg x16, x1, hi
    0xFA478820u, // 004C: ccmp x1, #0x7, #0x0, hi
    0x9A9F17F1u, // 0050: cset x17, eq
    0xAA0003F2u, // 0054: mov x18, x0
    0x8A210013u, // 0058: bic x19, x0, x1
    0x8B010814u, // 005C: add x20, x0, x1, lsl #2
    0xCB214015u, // 0060: sub x21, x0, w1, uxtw
    0x9AC12036u, // 0064: lsl x22, x1, x1
    0xB1000517u, // 0068: adds x23, x8, #0x1
    0x9A1F03F8u, // 006C: adc x24, xzr, xzr
    0xD4200000u, // 0070: brk #0
};

// load_store.S
alignas(16) inline const uint32_t load_store[] = {
    0xD2B20001u, // 0000: mov x1, #0x90000000 // =2415919104
    0xD2822442u, // 0004: mov x2, #0x1122 // =4386
    0xF2A66882u, // 0008: movk x2, #0x3344, lsl #16
    0xF2CAACC2u, // 000C: movk x2, #0x5566, lsl #32
    0xF2EEF102u, // 0010: movk x2, #0x7788, lsl #48
    0xF9000022u, // 0014: str x2, [x1]
    0xF9400023u, // 0018: ldr x3, [x1]
    0x39400024u, // 001C: ldrb w4, [x1]
    0x79400425u, // 0020: ldrh w5, [x1, #0x2]
    0xB9400426u, // 0024: ldr w6, [x1, #0x4]
    0x92800027u, // 0028: mov x7, #-0x2 // =-2
    0xB9000827u, // 002C: str w7, [x1, #0x8]
    0xB9800828u, // 0030: ldrsw x8, [x1, #0x8]
    0x39802029u, // 0034: ldrsb x9, [x1, #0x8]
    0x3940202Au, // 0038: ldrb w10, [x1, #0x8]
    0xF8010C22u, // 003C: str x2, [x1, #0x10]!
    0xF840842Bu, // 0040: ldr x11, [x1], #0x8
    0x9280000Cu, // 0044: mov x12, #-0x1 // =-1
    0xF86C782Du, // 0048: ldr x13, [x1, x12, lsl #3]
    0xF85F802Eu, // 004C: ldur x14, [x1, #-0x8]
    0x580000CFu, // 0050: ldr x15, 0x68 <literal>
    0x29041424u, // 0054: stp w4, w5, [x1, #0x20]
    0x29444430u, // 0058: ldp w16, w17, [x1, #0x20]
    0x697E4C32u, // 005C: ldpsw x18, x19, [x1, #-0x10]
    0xD4200000u, // 0060: brk #0
    0xD503201Fu, // 0064: nop
    0x89ABCDEFu, // 0068: .word 0x89abcdef
    0x01234567u, // 006C: .word 0x01234567
};

// loop_flags.S
alignas(16) inline const uint32_t loop_flags[] = {
    0xD2800000u, // 0000: mov x0, #0x0 // =0
    0xD2800141u, // 0004: mov x1, #0xa // =10
    0x8B010000u, // 0008: add x0, x0, x1
    0xF1000421u, // 000C: subs x1, x1, #0x1
    0x54FFFFC1u, // 0010: b.ne 0x8 <.text+0x8>
    0x92800082u, // 0014: mov x2, #-0x5 // =-5
    0xF1000C5Fu, // 0018: cmp x2, #0x3
    0x9A9FA7E3u, // 001C: cset x3, lt
    0x9A9F27E4u, // 0020: cset x4, lo
    0xB100145Fu, // 0024: cmn x2, #0x5
    0x9A9F17E5u, // 0028: cset x5, eq
    0xD4200000u, // 002C: brk #0
};

// mov_wide.S
alignas(16) inline const uint32_t mov_wide[] = {
    0xD2824680u, // 0000: mov x0, #0x1234 // =4660
    0xF2B579A0u, // 0004: movk x0, #0xabcd, lsl #16
    0xF2EACF00u, // 0008: movk x0, #0x5678, lsl #48
    0x92800001u, // 000C: mov x1, #-0x1 // =-1
    0x92800002u, // 0010: mov x2, #-0x1 // =-1
    0x12800002u, // 0014: mov w2, #-0x1 // =-1
    0x52BFFFE3u, // 0018: mov w3, #-0x10000 // =-65536
    0xD4200000u, // 001C: brk #0
};

// system.S
alignas(16) inline const uint32_t system[] = {
    0xD2800540u, // 0000: mov x0, #0x2a // =42
    0xD40004C1u, // 0004: svc #0x26
    0xD53BD061u, // 0008: mrs x1, TPIDRRO_EL0
    0xD53BE002u, // 000C: mrs x2, CNTFRQ_EL0
    0xD2824683u, // 0010: mov x3, #0x1234 // =4660
    0xD51BD043u, // 0014: msr TPIDR_EL0, x3
    0xD53BD044u, // 0018: mrs x4, TPIDR_EL0
    0xEB03007Fu, // 001C: cmp x3, x3
    0xD53B4205u, // 0020: mrs x5, NZCV
    0xD5033BBFu, // 0024: dmb ish
    0xD503201Fu, // 0028: nop
    0xD4200000u, // 002C: brk #0
};

// c_functions.c
alignas(16) inline const uint32_t c_functions[] = {
    0x34000160u, // 0000: cbz w0, 0x2c <fibonacci+0x2c>
    0xAA1F03E9u, // 0004: mov x9, xzr
    0x52800028u, // 0008: mov w8, #0x1 // =1
    0x8B08012Au, // 000C: add x10, x9, x8
    0x71000400u, // 0010: subs w0, w0, #0x1
    0xAA0803E1u, // 0014: mov x1, x8
    0xAA0803E9u, // 0018: mov x9, x8
    0xAA0A03E8u, // 001C: mov x8, x10
    0x54FFFF61u, // 0020: b.ne 0xc <fibonacci+0xc>
    0xAA0103E0u, // 0024: mov x0, x1
    0xD65F03C0u, // 0028: ret
    0xAA1F03E1u, // 002C: mov x1, xzr
    0xAA0103E0u, // 0030: mov x0, x1
    0xD65F03C0u, // 0034: ret
    0xF100081Fu, // 0038: cmp x0, #0x2
    0x54000062u, // 003C: b.hs 0x48 <factorial_rec+0x10>
    0x52800020u, // 0040: mov w0, #0x1 // =1
    0xD65F03C0u, // 0044: ret
    0xF100081Fu, // 0048: cmp x0, #0x2
    0x54000081u, // 004C: b.ne 0x5c <factorial_rec+0x24>
    0xAA0003E8u, // 0050: mov x8, x0
    0x52800020u, // 0054: mov w0, #0x1 // =1
    0x14000010u, // 0058: b 0x98 <factorial_rec+0x60>
    0xD1000409u, // 005C: sub x9, x0, #0x1
    0x5280002Bu, // 0060: mov w11, #0x1 // =1
    0x5280002Du, // 0064: mov w13, #0x1 // =1
    0x927FF92Au, // 0068: and x10, x9, #0xfffffffffffffffe
    0xCB0A0008u, // 006C: sub x8, x0, x10
    0xAA0A03ECu, // 0070: mov x12, x10
    0xD100040Eu, // 0074: sub x14, x0, #0x1
    0x9B0B7C0Bu, // 0078: mul x11, x0, x11
    0xD100098Cu, // 007C: sub x12, x12, #0x2
    0xD1000800u, // 0080: sub x0, x0, #0x2
    0x9B0D7DCDu, // 0084: mul x13, x14, x13
    0xB5FFFF6Cu, // 0088: cbnz x12, 0x74 <factorial_rec+0x3c>
    0x9B0B7DA0u, // 008C: mul x0, x13, x11
    0xEB0A013Fu, // 0090: cmp x9, x10
    0x540000A0u, // 0094: b.eq 0xa8 <factorial_rec+0x70>
    0x9B007D00u, // 0098: mul x0, x8, x0
    0xF100091Fu, // 009C: cmp x8, #0x2
    0xD1000508u, // 00A0: sub x8, x8, #0x1
    0x54FFFFA8u, // 00A4: b.hi 0x98 <factorial_rec+0x60>
    0xD65F03C0u, // 00A8: ret
    0xAA1F03E8u, // 00AC: mov x8, xzr
    0x38686809u, // 00B0: ldrb w9, [x0, x8]
    0x91000508u, // 00B4: add x8, x8, #0x1
    0x35FFFFC9u, // 00B8: cbnz w9, 0xb0 <string_length+0x4>
    0xD1000500u, // 00BC: sub x0, x8, #0x1
    0xD65F03C0u, // 00C0: ret
    0x34000301u, // 00C4: cbz w1, 0x124 <bubble_sort+0x60>
    0x2A1F03E8u, // 00C8: mov w8, wzr
    0x91002009u, // 00CC: add x9, x0, #0x8
    0x5100042Au, // 00D0: sub w10, w1, #0x1
    0x14000005u, // 00D4: b 0xe8 <bubble_sort+0x24>
    0x11000508u, // 00D8: add w8, w8, #0x1
    0x5100054Au, // 00DC: sub w10, w10, #0x1
    0x6B01011Fu, // 00E0: cmp w8, w1
    0x54000200u, // 00E4: b.eq 0x124 <bubble_sort+0x60>
    0x4B08002Bu, // 00E8: sub w11, w1, w8
    0x2A0A03EAu, // 00EC: mov w10, w10
    0xAA0903ECu, // 00F0: mov x12, x9
    0x7100097Fu, // 00F4: cmp w11, #0x2
    0xAA0A03EBu, // 00F8: mov x11, x10
    0x540000A2u, // 00FC: b.hs 0x110 <bubble_sort+0x4c>
    0x17FFFFF6u, // 0100: b 0xd8 <bubble_sort+0x14>
    0xF100056Bu, // 0104: subs x11, x11, #0x1
    0x9100218Cu, // 0108: add x12, x12, #0x8
    0x54FFFE60u, // 010C: b.eq 0xd8 <bubble_sort+0x14>
    0xA97FB98Du, // 0110: ldp x13, x14, [x12, #-0x8]
    0xEB0E01BFu, // 0114: cmp x13, x14
    0x54FFFF6Du, // 0118: b.le 0x104 <bubble_sort+0x40>
    0xA93FB58Eu, // 011C: stp x14, x13, [x12, #-0x8]
    0x17FFFFF9u, // 0120: b 0x104 <bubble_sort+0x40>
    0xD65F03C0u, // 0124: ret
    0xB40000E1u, // 0128: cbz x1, 0x144 <gcd+0x1c>
    0x9AC10809u, // 012C: udiv x9, x0, x1
    0xAA0103E8u, // 0130: mov x8, x1
    0x9B018121u, // 0134: msub x1, x9, x1, x0
    0xAA0803E0u, // 0138: mov x0, x8
    0xB5FFFF81u, // 013C: cbnz x1, 0x12c <gcd+0x4>
    0xAA0803E0u, // 0140: mov x0, x8
    0xD65F03C0u, // 0144: ret
    0x71000C1Fu, // 0148: cmp w0, #0x3
    0x540002C3u, // 014C: b.lo 0x1a4 <count_primes+0x5c>
    0x2A1F03E8u, // 0150: mov w8, wzr
    0x52800049u, // 0154: mov w9, #0x2 // =2
    0x14000006u, // 0158: b 0x170 <count_primes+0x28>
    0x5280002Au, // 015C: mov w10, #0x1 // =1
    0x11000529u, // 0160: add w9, w9, #0x1
    0x0B080148u, // 0164: add w8, w10, w8
    0x6B00013Fu, // 0168: cmp w9, w0
    0x54000200u, // 016C: b.eq 0x1ac <count_primes+0x64>
    0x7100113Fu, // 0170: cmp w9, #0x4
    0x54FFFF43u, // 0174: b.lo 0x15c <count_primes+0x14>
    0x5280004Au, // 0178: mov w10, #0x2 // =2
    0x1ACA092Bu, // 017C: udiv w11, w9, w10
    0x1B0AA56Bu, // 0180: msub w11, w11, w10, w9
    0x340000CBu, // 0184: cbz w11, 0x19c <count_primes+0x54>
    0x1100054Au, // 0188: add w10, w10, #0x1
    0x1B0A7D4Bu, // 018C: mul w11, w10, w10
    0x6B09017Fu, // 0190: cmp w11, w9
    0x54FFFF49u, // 0194: b.ls 0x17c <count_primes+0x34>
    0x17FFFFF1u, // 0198: b 0x15c <count_primes+0x14>
    0x2A1F03EAu, // 019C: mov w10, wzr
    0x17FFFFF0u, // 01A0: b 0x160 <count_primes+0x18>
    0x2A1F03E0u, // 01A4: mov w0, wzr
    0xD65F03C0u, // 01A8: ret
    0x2A0803E0u, // 01AC: mov w0, w8
    0xD65F03C0u, // 01B0: ret
    0xAA0003E8u, // 01B4: mov x8, x0
    0x5293B8A0u, // 01B8: mov w0, #0x9dc5 // =40389
    0x72B02380u, // 01BC: movk w0, #0x811c, lsl #16
    0xB4000101u, // 01C0: cbz x1, 0x1e0 <hash_fnv1a+0x2c>
    0x52803269u, // 01C4: mov w9, #0x193 // =403
    0x72A02009u, // 01C8: movk w9, #0x100, lsl #16
    0x3840150Au, // 01CC: ldrb w10, [x8], #0x1
    0xF1000421u, // 01D0: subs x1, x1, #0x1
    0x4A0A000Au, // 01D4: eor w10, w0, w10
    0x1B097D40u, // 01D8: mul w0, w10, w9
    0x54FFFF81u, // 01DC: b.ne 0x1cc <hash_fnv1a+0x18>
    0xD65F03C0u, // 01E0: ret
};
inline constexpr uint64_t c_functions_fibonacci = 0x0;
inline constexpr uint64_t c_functions_factorial_rec = 0x38;
inline constexpr uint64_t c_functions_string_length = 0xAC;
inline constexpr uint64_t c_functions_bubble_sort = 0xC4;
inline constexpr uint64_t c_functions_gcd = 0x128;
inline constexpr uint64_t c_functions_count_primes = 0x148;
inline constexpr uint64_t c_functions_hash_fnv1a = 0x1B4;

} // namespace NeXo2::Tests::Programs
