// Comun a todas las pruebas SIMD. END termina la prueba:
//  - en NeXo es "brk #0"
//  - en QEMU (referencia) salta al codigo que guarda los registros
#ifdef REFERENCE
#define END b snippet_end
#else
#define END brk #0
#endif
// Carga los registros v0..v7 con datos variados desde la tabla 'pattern'
.macro LOAD_PATTERN
    adr  x9, pattern
    ldp  q0, q1, [x9]
    ldp  q2, q3, [x9, #32]
    ldp  q4, q5, [x9, #64]
    ldp  q6, q7, [x9, #96]
    mov  x9, #0
.endm
.macro PATTERN_DATA
    .p2align 4
pattern:
    .quad 0x0123456789ABCDEF, 0xFEDCBA9876543210
    .quad 0x8000000180000001, 0x7FFFFFFF00000000
    .quad 0x00FF00FF7F807F80, 0xFFFFFFFFFFFFFFFF
    .quad 0x1122334455667788, 0x99AABBCCDDEEFF00
    .quad 0x0102030405060708, 0x090A0B0C0D0E0F10
    .quad 0xF0E0D0C0B0A09080, 0x7060504030201000
    .quad 0x4045000000000000, 0xC00921FB54442D18   // 42.0, -3.14159...
    .quad 0x3FC000003F800000, 0xC2C80000BF000000   // floats: 1.0, 1.5, -0.5, -100.0
.endm
