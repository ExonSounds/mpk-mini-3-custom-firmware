# RAM programs: make -C programs  ->  programs/<name>.bin, run with: python3 mpk3_cfg.py run programs/<name>.bin
# Each <name>.c with int program(const prog_api_t *api) becomes one program (API: ../fw/prog_api.h).
CFLAGS = --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffreestanding -fno-builtin \
         -nostdlib -Wall -Wextra -ffunction-sections -fdata-sections -I../fw
PROGS = $(filter-out start,$(basename $(wildcard *.c)))

all: $(PROGS:=.bin)

%.o: %.c ../fw/prog_api.h Makefile
	clang $(CFLAGS) -c $< -o $@

%.elf: %.o start.o prog.ld
	ld.lld -T prog.ld --gc-sections -e prog_header start.o $< -o $@

%.bin: %.elf
	llvm-objcopy -O binary $< $@

clean:
	rm -f *.o *.elf *.bin

.PRECIOUS: %.elf %.o
