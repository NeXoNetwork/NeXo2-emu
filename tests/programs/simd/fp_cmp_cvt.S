// Comparaciones (flags), fcsel, fccmp, conversiones entero <-> coma flotante
#include "common.inc"
    LOAD_PATTERN
    fmov d8, #1.5
    fmov d9, #-2.75
    fcmp d8, d9
    mrs  x0, nzcv
    fcmp d9, d8
    mrs  x1, nzcv
    fcmp d8, d8
    mrs  x2, nzcv
    fcmp d8, #0.0
    mrs  x3, nzcv
    fcmpe d9, #0.0
    mrs  x4, nzcv
    mov  x5, #0x7FF8000000000000      // NaN
    fmov d10, x5
    fcmp d10, d8
    mrs  x5, nzcv
    fcmp s7, s7
    mrs  x6, nzcv
    fcmp d8, d9
    fcsel d11, d8, d9, gt
    fcsel d12, d8, d9, lt
    fccmp d8, d8, #0b1010, eq
    mrs  x7, nzcv
    fccmp d8, d9, #0b1010, ne
    mrs  x8, nzcv
    // entero -> coma flotante
    mov  w9, #-7
    scvtf d13, w9
    ucvtf d14, w9
    movz x10, #0x4240
    movk x10, #0xF, lsl #16          // 1000000
    scvtf s15, x10
    ucvtf d16, x10
    mov  x11, #-1
    ucvtf d17, x11
    scvtf d18, x11
    // coma flotante -> entero (redondeos y saturacion)
    fcvtzs w12, d9                    // -2
    fcvtzu w13, d9                    // 0
    fcvtms w14, d9                    // -3
    fcvtps w15, d9                    // -2
    fcvtns x16, d8                    // 2 (1.5 -> par)
    fcvtas x17, d9                    // -3
    mov  x18, #0x43E0000000000000     // 2^63
    fmov d19, x18
    fcvtzs x19, d19                   // satura a INT64_MAX
    fcvtzu x20, d19                   // 2^63
    fcvtzs w21, d10                   // NaN -> 0
    fcvtzs w22, s7                    // -0.5f -> 0
    fcvtzu x23, d6                    // 42
    // versiones SIMD escalares
    scvtf d24, d0
    fcvtzs d25, d6
    ucvtf s26, s3
    END
    PATTERN_DATA
