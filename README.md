# MPK mini 3 custom firmware

Open-source replacement firmware for the **Akai MPK mini 3** (the Artery **AT32F415RBT7** hardware variant), plus
Linux tools to flash it over USB and to edit programs on the stock firmware.

The custom firmware is a class-compliant USB-MIDI device (no drivers) that you configure from the keyboard's own
buttons and screen, or from a desktop GUI (`mpk3_gui.py`) that edits every setting live.

> **Not affiliated with Akai Professional or inMusic.** "Akai" and "MPK" are their trademarks. Flashing custom
> firmware may void your warranty. Use at your own risk; see the [licence](LICENSE).

## Features (firmware v9.30)

- **Keys:** 25 keys with velocity from the two-contact timing, octave −4…+4 (OCT−/OCT+, both = reset),
  transpose ±12, velocity curves (soft / normal / hard / fixed).
- **Pads:** 8 velocity-sensitive pads, bank A/B, FULL LEVEL (keys and pads at 127), note / CC / program-change
  modes with a note, CC and program per pad (16 pads), channel or polyphonic aftertouch, pad LEDs.
- **Knobs:** 8 endless knobs, each with its own CC, min/max range (reversible) and absolute or relative mode;
  three speeds; own MIDI channel shared with the joystick.
- **Joystick:** per axis pitch bend, one CC over the whole range, or one CC per direction.
- **Arpeggiator:** up, down, up/down (exclusive/inclusive), as played, random; 1–4 octaves; 1/4–1/32 incl.
  triplets; latch; swing and gate length.
- **Note repeat** for pads.
- **Tempo:** internal clock with TAP TEMPO (the LED blinks on the beat), or **external MIDI clock** with
  Start/Continue/Stop from your DAW.
- **On-device menu:** PROG SELECT opens/closes it (closing saves to flash), OCT−/OCT+ pick a setting, knob 1
  changes it. Settings are kept across power cycles.
- **Desktop GUI and command line** over USB-MIDI SysEx: read, edit live, save to the keyboard, defaults,
  backup/restore to a JSON file.
- **Live screens:** the keyboard with held keys (shaded by velocity), the pad grid with held pads (shaded by live
  pressure), all 8 dials with pointer and value, and the joystick position.
- **Idle screen:** after a chosen idle time the keyboard plays ping-pong against itself, or the screen switches off.

Not done yet: saving more than one setup (presets), flashing from the GUI.

## Is it safe?

The keyboard's own **bootloader is never modified**. The flasher refuses any image whose first 40 KB (bootloader
and its copy) differ from stock. Holding **PAD BANK + PROG SELECT** while plugging in always enters update mode,
before any application code runs, so you can always go back to the stock firmware.

Only the AT32 variant (USB ID `09e8:1049` in normal mode) has been tested. The STM32 variant (`09e8:0049`) uses
a different image layout and is not supported.

## Requirements (Linux)

- `clang` and `lld` (Arch: `sudo pacman -S clang lld`), `python3`, `curl`, `unzip`
- For the GUI: Tk (Arch: `sudo pacman -S tk`)
- Permission to open the keyboard's USB HID port:

  ```bash
  sudo cp firmware/70-akai-mpk3.rules /etc/udev/rules.d/
  sudo udevadm control --reload
  ```

## Usage

```bash
firmware/get_stock_firmware.sh        # downloads Akai's updater v1.26, extracts the stock images (verified by SHA-1)
make -C fw                            # builds fw/mpk.bin (custom) and fw/test.bin (hardware test)
make -C fw check                      # host tests of the key, pad, knob, menu and sequencer logic
```

Put the keyboard in **update mode** (hold PAD BANK + PROG SELECT while plugging in USB), then:

```bash
python3 mpk3_flash.py fw/mpk.bin      # custom firmware
python3 mpk3_flash.py                 # back to stock v1.26
```

After flashing, the keyboard restarts on its own. Flashing resets the custom firmware's settings to defaults.

Configure the running custom firmware from the desktop (close DAWs that hold the MIDI port first):

```bash
python3 mpk3_gui.py                   # GUI: Connect, edit (applied live), Save to keyboard, files
python3 mpk3_cfg.py get               # or the command line: info | get | set KEY=VALUE ... | save | defaults
```

## Repository layout

| Path | What |
|------|------|
| `fw/` | Custom firmware (C, no vendor SDK): `board.*` clocks/pins/OLED/LEDs, `usb_midi.*` USB-MIDI driver, `analog.*` pads/knobs/joystick, `ui.*` buttons/LEDs, `menu.*` + `settings.*` on-device menu and flash storage, `seq.*` clock/arpeggiator/note repeat, `cfg.*` SysEx settings protocol, `gfx.*`/`views.*`/`pong.*` drawing, live screens and idle game, `test.c` hardware test firmware |
| `fw/mkimage.py` | Wraps the app into a full image with the stock bootloader region and a valid trailer |
| `mpk3_flash.py` | Flasher for the stock USB HID bootloader |
| `mpk3_gui.py`, `mpk3_cfg.py` | Desktop GUI and command line for the custom firmware's settings (SysEx protocol in `fw/cfg.h`) |
| `mpk3.py`, `mpk3_tui.py` | Read/write the 8 programs of the **stock** firmware over SysEx, with a terminal UI (backs up all programs first) |
| `firmware/NOTES.md` | Reverse-engineering notes: MCU, pin map, update protocol, image layout, hardware findings |
| `firmware/get_stock_firmware.sh` | Fetches the stock firmware from Akai's download server (it is not in this repo) |

## Tests

```bash
make -C fw check
python3 -m unittest test_mpk3 test_mpk3_cfg test_mpk3_flash   # test_mpk3_flash needs the stock firmware first
```

## Third-party

- **Akai's stock firmware and bootloader** are not included. `get_stock_firmware.sh` downloads them from Akai,
  and built images contain the stock bootloader region, so don't redistribute built `.bin` files.
- **Font:** the 5×7 character set in `fw/board.c` is the classic LCD font found in many libraries (it matches
  `glcdfont.c` in [Adafruit-GFX](https://github.com/adafruit/Adafruit-GFX-Library), BSD licence).
- **USB ID:** the firmware uses `1209:0001`, the [pid.codes](https://pid.codes) *test* PID. It is fine for
  personal use; a distributed build should apply for its own PID.

## Licence

[MIT](LICENSE), except where noted under Third-party.
