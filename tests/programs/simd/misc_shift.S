// Dos registros (misc), entre carriles y desplazamientos por inmediato
#include "common.inc"
    LOAD_PATTERN
    cmeq v8.16b, v4.16b, #0
    cmge v9.8h, v1.8h, #0
    cmgt v10.4s, v1.4s, #0
    cmle v11.16b, v0.16b, #0
    cmlt v12.2d, v1.2d, #0
    cmeq v13.2d, v2.2d, #0
    abs  v14.4s, v1.4s
    neg  v15.8h, v0.8h
    not  v16.16b, v0.16b
    cnt  v17.16b, v0.16b
    rbit v18.8b, v0.8b
    rev64 v19.4s, v0.4s
    rev32 v20.8h, v0.8h
    rev16 v21.16b, v0.16b
    xtn  v22.8b, v0.8h
    xtn2 v22.16b, v1.8h
    addv b23, v0.16b
    umaxv h24, v1.8h
    uminv s25, v0.4s
    smaxv b26, v4.16b
    sminv h27, v1.8h
    uaddlv h28, v0.16b
    saddlv d29, v1.4s
    shl  v30.4s, v0.4s, #3
    ushr v31.8h, v1.8h, #4
    sshr v2.16b, v4.16b, #2
    mov  v3.16b, v0.16b
    usra v3.2d, v1.2d, #60
    shrn v5.8b, v0.8h, #4
    shrn2 v5.16b, v1.8h, #8
    ushll v6.8h, v0.8b, #2
    uxtl2 v7.4s, v1.8h
    sshll v4.2d, v1.2s, #0
    shl  d0, d1, #12
    ushr d1, d2, #8
    sshr d2, d1, #63
    cmge d3, d0, #0
    END
    PATTERN_DATA
