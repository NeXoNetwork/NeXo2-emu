// DUP, INS, UMOV/SMOV, MOVI/MVNI/ORR/BIC inmediato, FMOV vector
#include "common.inc"
    LOAD_PATTERN
    mov  w0, #0x1234
    dup  v8.16b, w0
    dup  v9.8h, w0
    dup  v10.4s, w0
    dup  v11.2d, x0
    dup  v12.8b, w0
    dup  v13.16b, v0.b[3]
    dup  v14.4s, v1.s[2]
    dup  v15.2d, v2.d[1]
    mov  v16.16b, v0.16b
    mov  v16.s[1], w0
    mov  v16.d[1], x0
    mov  v16.b[15], v1.b[0]
    mov  v16.h[2], v1.h[7]
    mov  x1, v0.d[1]
    umov w2, v0.b[7]
    umov w3, v1.h[3]
    mov  w4, v1.s[3]
    smov x5, v1.b[8]
    smov w6, v1.h[7]
    smov x7, v1.s[1]
    movi v17.4s, #0x20
    movi v18.4s, #0x20, lsl #24
    movi v19.8h, #0x7F, lsl #8
    movi v20.16b, #0xA5
    movi v21.2d, #0xFF00FF0000FFFF00
    movi d22, #0xFF000000000000FF
    movi v23.2s, #0x11, msl #8
    movi v24.4s, #0x11, msl #16
    mvni v25.4s, #0x1
    mvni v26.2s, #0x80, lsl #8
    mvni v27.8h, #0x3
    mov  v28.16b, v0.16b
    orr  v28.4s, #0x80, lsl #16
    mov  v29.16b, v5.16b
    bic  v29.8h, #0xFF
    fmov v30.4s, #-1.25
    fmov v31.2d, #0.5
    fmov v7.2s, #3.0
    END
    PATTERN_DATA
