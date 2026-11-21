# make -C fw  ->  fw/mpk.bin (custom firmware) and fw/test.bin (hardware test)
# flash with: python3 mpk3_flash.py fw/mpk.bin   (keyboard in update mode: PAD BANK + PROG SELECT at plug-in)
VERSION = 0950
CFLAGS = -DFW_VERSION=0x$(VERSION) --target=thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -ffreestanding -fno-builtin \
         -nostdlib -Wall -Wextra -ffunction-sections

all: mpk.bin test.bin

%.o: %.c board.h usb_midi.h analog.h ui.h menu.h settings.h seq.h cfg.h pong.h views.h Makefile
	clang $(CFLAGS) -c $< -o $@

mpk.elf: board.o gfx.o usb_midi.o analog.o ui.o menu.o settings.o seq.o cfg.o pong.o views.o main.o link.ld
	ld.lld -T link.ld --gc-sections -e Reset_Handler board.o gfx.o usb_midi.o analog.o ui.o menu.o settings.o seq.o cfg.o pong.o views.o main.o -o $@

test.elf: board.o gfx.o test.o link.ld
	ld.lld -T link.ld --gc-sections -e Reset_Handler board.o gfx.o test.o -o $@

%.app: %.elf
	llvm-objcopy -O binary $< $@

mpk.bin: mpk.app mkimage.py ../mpk3_flash.py
	python3 mkimage.py $< $@ $(VERSION)

test.bin: test.app mkimage.py ../mpk3_flash.py
	python3 mkimage.py $< $@ 0001

check: test_keys.c main.c analog.c ui.c menu.c settings.c seq.c cfg.c pong.c views.c gfx.c
	cc -std=c11 -Wall -Wextra -Wno-unused-function -o test_keys test_keys.c -lm && ./test_keys

clean:
	rm -f *.o *.elf *.app *.bin test_keys
