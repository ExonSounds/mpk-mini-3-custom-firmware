/* RAM programs: one flat binary at PROG_BASE (fw/prog_api.h), header first. Code, data and bss must fit in 24 KB. */
MEMORY
{
  RAM (rwx) : ORIGIN = 0x20002000, LENGTH = 0x6000
}

SECTIONS
{
  .text : { KEEP(*(.prog_header)) *(.text*) *(.rodata*) *(.data*) . = ALIGN(4); } > RAM
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
}
