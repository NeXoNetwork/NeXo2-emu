// Coma flotante escalar: fmov, aritmetica, fusionadas, 1 operando, conversiones de tamano, frint
#include "common.inc"
    LOAD_PATTERN
    fmov d8, #1.5
    fmov d9, #-0.125
    fmov s10, #31.0
    fmov s11, #0.25
    mov  x0, #0x4000000000000000      // 2.0
    fmov d12, x0
    fmov x1, d6                       // 42.0 como bits
    mov  w2, #0x40400000              // 3.0f
    fmov s13, w2
    fmov w3, s7
    fmov v14.d[1], x0
    fmov x4, v6.d[1]
    fadd d16, d6, d8
    fsub d17, d8, d6
    fmul d18, d6, d9
    fdiv d19, d6, d12
    fnmul d20, d8, d12
    fmax d21, d8, d9
    fmin d22, d8, d9
    fmaxnm d23, d8, d6
    fminnm d24, d9, d6
    fadd s25, s10, s11
    fmul s26, s10, s13
    fdiv s27, s11, s13
    fsub s28, s11, s10
    fmadd d29, d6, d8, d12
    fmsub d30, d6, d8, d12
    fnmadd d31, d6, d8, d12
    fnmsub d1, d6, d8, d12
    fmadd s2, s10, s11, s13
    fabs d3, d9
    fneg d4, d8
    fsqrt d5, d12
    fsqrt s15, s10
    fcvt s0, d6
    fcvt d7, s10
    fneg s14, s13
    // redondeos de 2.5 y -2.5
    fmov d9, #2.5
    frintn d10, d9
    frintp d11, d9
    frintm d13, d9
    frinta d12, d9
    fneg  d9, d9
    frintz d8, d9
    END
    PATTERN_DATA
