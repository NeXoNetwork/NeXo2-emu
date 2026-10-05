// UZP, ZIP, TRN, EXT, TBL, TBX
#include "common.inc"
    LOAD_PATTERN
    uzp1 v8.16b, v0.16b, v1.16b
    uzp2 v9.8h, v0.8h, v1.8h
    uzp1 v10.4s, v2.4s, v3.4s
    zip1 v11.16b, v0.16b, v1.16b
    zip2 v12.4s, v0.4s, v1.4s
    zip1 v13.2d, v2.2d, v3.2d
    trn1 v14.8h, v0.8h, v1.8h
    trn2 v15.16b, v0.16b, v1.16b
    zip1 v16.8b, v0.8b, v1.8b
    ext  v17.16b, v0.16b, v1.16b, #5
    ext  v18.8b, v0.8b, v1.8b, #3
    ext  v19.16b, v2.16b, v3.16b, #15
    movi v20.16b, #0
    adr  x9, tbl_idx
    ldr  q21, [x9]
    tbl  v22.16b, {v4.16b}, v21.16b
    tbl  v23.16b, {v4.16b, v5.16b}, v21.16b
    mov  v24.16b, v0.16b
    tbx  v24.16b, {v4.16b}, v21.16b
    tbl  v25.8b, {v0.16b, v1.16b, v2.16b}, v21.8b
    mov  x9, #0
    END
    .p2align 4
tbl_idx: .byte 0, 15, 3, 31, 16, 200, 7, 8, 17, 1, 47, 2, 30, 64, 9, 5
    PATTERN_DATA
