/* Linker script del NRO de prueba: imagen plana desde 0, segmentos alineados a 4 KB. */
ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : { KEEP(*(.text.crt0)) *(.text .text.*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) }
    .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(8); __bss_end = .; }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
