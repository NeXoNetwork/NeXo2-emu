// Enteros vectoriales: add/sub/mul/mla/mls, comparaciones, max/min, pairwise, shifts, logicas
#include "common.inc"
    LOAD_PATTERN
    add  v8.16b, v0.16b, v1.16b
    add  v9.8h, v2.8h, v3.8h
    add  v10.4s, v4.4s, v5.4s
    add  v11.2d, v0.2d, v2.2d
    sub  v12.4s, v1.4s, v3.4s
    mul  v13.8h, v3.8h, v4.8h
    mov  v14.16b, v0.16b
    mla  v14.4s, v1.4s, v2.4s
    mov  v15.16b, v0.16b
    mls  v15.8b, v1.8b, v2.8b
    cmeq v16.16b, v0.16b, v4.16b
    cmgt v17.8h, v1.8h, v2.8h
    cmge v18.4s, v1.4s, v3.4s
    cmhi v19.16b, v0.16b, v5.16b
    cmhs v20.16b, v0.16b, v0.16b
    cmtst v21.4s, v1.4s, v2.4s
    umax v22.16b, v0.16b, v1.16b
    smax v23.8h, v0.8h, v1.8h
    umin v24.4s, v2.4s, v3.4s
    smin v25.4s, v2.4s, v3.4s
    addp v26.16b, v0.16b, v1.16b
    addp v27.2d, v2.2d, v3.2d
    umaxp v28.16b, v4.16b, v5.16b
    uminp v29.16b, v4.16b, v5.16b
    umaxp v30.4s, v1.4s, v2.4s
    sminp v31.8h, v1.8h, v2.8h
    // estos usan los resultados de arriba
    and  v6.16b, v8.16b, v9.16b
    bic  v7.16b, v8.16b, v10.16b
    orr  v8.8b, v9.8b, v10.8b
    orn  v9.16b, v10.16b, v11.16b
    eor  v10.16b, v11.16b, v12.16b
    mov  v11.16b, v0.16b
    bsl  v11.16b, v1.16b, v2.16b
    mov  v12.16b, v0.16b
    bit  v12.16b, v1.16b, v2.16b
    mov  v13.16b, v0.16b
    bif  v13.16b, v1.16b, v2.16b
    movi v3.16b, #3
    ushl v4.16b, v0.16b, v3.16b
    movi v5.16b, #0xFE                // -2
    sshl v5.8h, v1.8h, v5.8h
    // escalares de 64 bits
    add  d0, d1, d2
    sub  d1, d2, d0
    cmeq d2, d0, d0
    addp d3, v11.2d
    END
    PATTERN_DATA
