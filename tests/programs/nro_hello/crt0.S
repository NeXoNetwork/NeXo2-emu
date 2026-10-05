// Arranque (crt0) de un NRO minimo. Sustituye al de libnx para las pruebas.
// Los primeros 0x80 bytes son la cabecera del NRO; tools/make_nro.py la rellena.
    .section .text.crt0, "ax"
    .global _start
_start:
    b     entry                 // 0x00: RocrtHeader -> salta al codigo real
    .word 0                     // 0x04: offset de MOD0 (no lo usamos)
    .word 0, 0                  // 0x08: relleno
    .space 0x70                 // 0x10: NroHeader (lo escribe make_nro.py)

entry:
    // Al entrar: X0 = config del loader, X1 = -1, X30 = vuelta al loader (svcExitProcess)
    stp   x29, x30, [sp, #-16]!
    mov   x29, sp

    // Poner .bss a cero (el kernel ya lo hace, pero un crt0 real no se fia)
    adrp  x2, __bss_start
    add   x2, x2, :lo12:__bss_start
    adrp  x3, __bss_end
    add   x3, x3, :lo12:__bss_end
1:  cmp   x2, x3
    b.hs  2f
    strb  wzr, [x2], #1
    b     1b

2:  bl    main                  // main(config, -1)
    ldp   x29, x30, [sp], #16
    ret                         // vuelve al loader -> svcExitProcess
