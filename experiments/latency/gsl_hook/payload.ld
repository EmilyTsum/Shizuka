ENTRY(shizuka_gsl_gateway)
PHDRS { payload PT_LOAD FLAGS(5); }
SECTIONS {
  . = 0x48000;
  .text : ALIGN(16) { KEEP(*(.text.shizuka_gateway)) *(.text .text.*) } :payload
  .rodata : ALIGN(16) { *(.rodata .rodata.*) } :payload
  .data : ALIGN(16) { *(.data .data.*) } :payload
  .bss : ALIGN(16) { *(.bss .bss.*) *(COMMON) } :payload
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
