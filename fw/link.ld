/* App area of the AT32F415RB: 0x0800A000 up to the settings sector at 0x0801E000 (settings.h); boot region
 * below stays stock, trailer at 0x0801FFF0. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x0800A000, LENGTH = 0x14000
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x4B70  /* same top of stack as stock */
}

SECTIONS
{
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
