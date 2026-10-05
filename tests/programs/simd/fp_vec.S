// Coma flotante vectorial (4s, 2d, 2s)
#include "common.inc"
    LOAD_PATTERN
    fadd v8.4s, v7.4s, v7.4s
    fsub v9.4s, v7.4s, v6.4s
    fmul v10.2d, v6.2d, v6.2d
    fdiv v11.2d, v6.2d, v6.2d
    fmul v12.2s, v7.2s, v7.2s
    mov  v13.16b, v7.16b
    fmla v13.4s, v7.4s, v7.4s
    mov  v14.16b, v6.16b
    fmls v14.2d, v6.2d, v6.2d
    fmax v15.4s, v7.4s, v8.4s
    fmin v16.2d, v6.2d, v10.2d
    faddp v17.4s, v7.4s, v8.4s
    faddp v18.2d, v6.2d, v10.2d
    fcmeq v19.4s, v7.4s, v7.4s
    fcmge v20.4s, v7.4s, v8.4s
    fcmgt v21.2d, v10.2d, v6.2d
    fabs v22.4s, v7.4s
    fneg v23.2d, v6.2d
    fsqrt v24.2d, v10.2d
    scvtf v25.4s, v1.4s
    ucvtf v26.2d, v0.2d
    fcvtzs v27.4s, v7.4s
    fcvtzu v28.2d, v6.2d
    fcvtzs v29.2d, v10.2d
    END
    PATTERN_DATA
