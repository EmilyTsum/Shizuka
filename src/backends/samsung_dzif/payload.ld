PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x2000000;
 .text : { KEEP(*(.text.anchor)) *(.text*) *(.rodata*) } :code
 . = ALIGN(0x4000);
 .data : { *(.data*) *(.bss*) *(COMMON) } :data
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
