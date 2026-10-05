// Cargas y guardados SIMD: ldr/str (b,h,s,d,q), ldp/stp, pre/post-indice, ldur/stur,
// registro + desplazamiento, ld1/st1 (1-4 registros y carril), ldr literal
#include "common.inc"
    LOAD_PATTERN
    sub  x10, sp, #512          // zona de trabajo en la pila (la direccion no se compara)
    stp  q0, q1, [x10]
    stp  q2, q3, [x10, #32]
    str  q4, [x10, #64]
    str  d5, [x10, #80]
    str  s6, [x10, #88]
    str  h7, [x10, #92]
    str  b0, [x10, #94]
    ldr  q8, [x10, #16]
    ldr  d9, [x10, #40]
    ldr  s10, [x10, #4]
    ldr  h11, [x10, #6]
    ldr  b12, [x10, #9]
    ldp  q13, q14, [x10, #64]
    ldp  d15, d16, [x10, #8]
    ldp  s17, s18, [x10, #12]
    ldur q19, [x10, #3]
    stur d1, [x10, #-8]
    ldur d20, [x10, #-8]
    mov  x11, x10
    ldr  q21, [x11, #16]!       // pre-indice
    ldr  q22, [x11], #-16       // post-indice
    sub  x0, x11, x10           // = 0
    mov  w12, #2
    ldr  d23, [x10, w12, sxtw #3]
    mov  x12, #3
    ldr  q24, [x10, x12, lsl #4]
    str  d2, [x10, x12, lsl #3]
    ldr  x1, [x10, #24]
    add  x13, x10, #128
    st1  {v0.16b, v1.16b, v2.16b, v3.16b}, [x13], #64
    sub  x13, x13, #64
    ld1  {v25.16b, v26.16b}, [x13]
    ld1  {v27.4s}, [x13], #16
    sub  x2, x13, x10           // = 144
    ld1  {v28.b}[6], [x10]
    ld1  {v28.s}[3], [x13]
    st1  {v3.d}[1], [x10]
    ldr  x3, [x10]
    ldr  q29, lit_q
    ldr  d30, lit_d
    ldr  s31, lit_s
    stp  q5, q6, [x10, #-32]!   // pre-indice par
    ldp  q4, q5, [x10], #32     // post-indice par
    mov  x10, #0
    mov  x11, #0
    mov  x13, #0
    END
    .p2align 4
lit_q: .quad 0xAAAAAAAABBBBBBBB, 0xCCCCCCCCDDDDDDDD
lit_d: .quad 0x123456789ABCDEF0
lit_s: .word 0x40490FDB
    PATTERN_DATA
