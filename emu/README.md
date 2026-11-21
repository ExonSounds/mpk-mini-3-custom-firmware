# MPK mini 3 emulator

Runs the keyboard's firmware images — Akai's stock firmware and this repo's custom firmware — instruction by
instruction on an emulated AT32F415RB (Cortex-M4, 128 KB flash, 32 KB SRAM), with models of every peripheral
they touch. Everything the CPU does is exact because it is the same code; analog behaviour (pad pressure, knob
wipers, key contact timing, joystick) is modelled from `firmware/NOTES.md` and is approximate.

```bash
sudo pacman -S unicorn python-unicorn   # CPU engine (Arch); alsa-lib and tk are usually installed already
python3 mpk3_emu.py                     # custom firmware (builds emu/ and fw/ first), with the front panel
python3 mpk3_emu.py stock               # Akai's firmware (fetch it first: firmware/get_stock_firmware.sh)
python3 mpk3_emu.py custom --reset      # start from a freshly flashed image
```

While it runs, the repo's tools find it before a real keyboard (`mpk3_cfg.py`, `mpk3_gui.py`, `mpk3_market.py`,
`mpk3.py`, `mpk3_tui.py`, `mpk3_flash.py`), and DAWs see an ALSA sequencer port named like the device
("MPK mini 3 custom" / "MPK mini 3"). Settings and anything else the firmware stores in flash persist between runs
in `~/.local/share/mpk3-emu/<image>.flash`. A rebuilt custom firmware is flashed on the next start, which wipes
its settings, as on the keyboard.

**Front panel** (`emu/panel.py`): click keys (lower = harder, Shift = 127) or type them (`Z S X D C ...` and
`Q 2 W 3 E ...`, velocity slider); click pads (drag or wheel while held = pressure); wheel or drag knobs; drag the
joystick; right-click latches a button/key/pad so combos work. **Update mode:** latch PAD BANK and PROG SELECT,
then POWER CYCLE; `python3 mpk3_flash.py fw/mpk.bin` then flashes the emulator through Akai's own bootloader.

## How it works

| File | What |
|------|------|
| `core.c`, `core.h` | Unicorn CPU, memory map (flash + alias at 0, SRAM, system page, bit-band), MMIO dispatch, events in CPU cycles (1 instruction = 1 cycle at 48 MHz), NVIC/SCB/SysTick/DWT, exception entry/return (Unicorn has no NVIC: entry happens between blocks, return arrives as EXCP_EXCEPTION_EXIT), faults, resets, real-time pacing. `core.h` is the interface the models are written against. |
| `sysctl.c` | RCC (clocks report ready), PWC/IOMUX/EXINT as plain registers, flash controller and option bytes on the mmap'ed state file |
| `gpio.c` | GPIO A–D with output watchers and input suppliers |
| `inputs.c` | key/button matrix (two timed contacts per key), analog inputs behind the muxes, pedal |
| `adc.c`, `dma.c`, `spi.c`, `timers.c` | ADC1 (single + DMA scan), DMA1, SPI1/SPI2 (TX + DMA), TMR1–5 |
| `oled.c`, `leds.c` | SSD1306 decoder (bit-banged pins and SPI2; which 32 of its 64 rows the glass shows is explained at the top), LED shift registers with PWM brightness |
| `usb.c`, `usbhost.c`, `usb.h` | DWC2 OTG FS device model and an emulated Linux-like host (enumeration, 1 ms frames, bulk/interrupt polling) |
| `midi_bridge.c` | ALSA sequencer port, raw-MIDI pty (`$XDG_RUNTIME_DIR/mpk3-emu/midi`, product string in `name`), hidraw-like pty (`hid`) |
| `panel_srv.c` | Unix socket `$XDG_RUNTIME_DIR/mpk3-emu/panel.sock`; text protocol documented at the top |
| `panel.py`, `emu_client.py` | Tk front panel and the socket client library (also used by the tests) |

`emu/mpk3emu FLASHFILE [--image IMAGE] [--fast] [--seconds S] [--run-dir DIR] [--profile] [-v]` is the core on
its own: `--run-dir` keeps several emulators apart (`MPK3_MIDI=DIR/midi` points the tools at one), `--profile`
prints the hottest code blocks, and register accesses no model handles are logged once each (neither image
produces any).

## Approximations

- Timing: 1 instruction = 1 cycle; flash erase 6 ms, halfword program 42 µs; DMA moves are instant; timers count
  up only; the host's USB timing is Linux-like but simplified (no suspend). On a busy machine real-time pacing
  may fall behind (it then runs as fast as it can, and MIDI timing stretches accordingly).
- Inputs: key contact time is the inverse of the custom firmware's velocity curve (stock applies its own); pads
  step to a level (force 1–127 → ADC 450–3800) with no rise time or noise; knobs turn at most 360°/s; the
  joystick spans 2048 ± 1900; pedal pressed = PA5 low (a guess).
- Not modelled: USB data toggles, suspend/resume, SPI receive, OLED scrolling.

## Tests

```bash
make -C emu check     # panel client, peripherals (both images), USB/MIDI/tools (both images, incl. update mode)
```

Stock tests are skipped when `firmware/stock_v1.26/AT.bin` is missing. Tests run their own emulators in temporary
run directories, so they don't disturb one that is running.
