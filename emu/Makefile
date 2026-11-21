# make -C emu  ->  emu/mpk3emu (emulator core); make -C emu check runs the host tests.
# Needs the Unicorn engine (Arch: unicorn) and ALSA (alsa-lib).
CFLAGS = -O2 -g -Wall -Wextra -std=gnu11
LDLIBS = -lunicorn -lpthread -lm -lasound
SRC = main.c core.c sysctl.c gpio.c panel_srv.c usb.c usbhost.c midi_bridge.c \
      inputs.c adc.c dma.c spi.c timers.c oled.c leds.c
HDR = core.h usb.h periph.h

mpk3emu: $(SRC) $(HDR) Makefile
	cc $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

check: mpk3emu
	python3 -m unittest discover -s . -p 'test_*.py'

clean:
	rm -f mpk3emu
