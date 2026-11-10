ENTRY(shizuka_runtime_acdb_gateway)
shizuka_original_cfi_continue = 0x19008;
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x48000;
  .text : ALIGN(16) { KEEP(*(.text.shizuka_runtime_acdb_gateway)) *(.text .text.*) } :text
  .rodata : ALIGN(16) { *(.rodata .rodata.*) } :text
  . = ALIGN(0x4000);
  .data : ALIGN(64) { *(.data .data.*) } :data
  .bss : ALIGN(64) { *(.bss .bss.*) *(COMMON) } :data
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
