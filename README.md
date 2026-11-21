# MPK mini 3 custom firmware

Open-source replacement firmware for the **Akai MPK mini 3** (the Artery **AT32F415RBT7** hardware variant), plus
Linux tools to flash it over USB and to edit programs on the stock firmware.

The custom firmware is a class-compliant USB-MIDI device (no drivers) that you configure from the keyboard's own
buttons and screen, or from a desktop GUI (`mpk3_gui.py`) that edits every setting live.

> **Not affiliated with Akai Professional or inMusic.** "Akai" and "MPK" are their trademarks. Flashing custom
> firmware may void your warranty. Use at your own risk; see the [licence](LICENSE).

## Features (firmware v10.00)

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
- **Home screen:** shows octave, pad bank and mode, arp, note repeat, full level, clock source, USB and tempo;
  pressing a button shows a short ON/OFF message, and TAP TEMPO shows the BPM as you tap (or the external clock's).
- **Idle screen:** after a chosen idle time the keyboard plays ping-pong against itself, shows a static picture
  (inverted every minute against burn-in), or switches the screen and all LEDs off. Pick it in the menu or the GUI.
- **Idle picture:** draw it pixel by pixel in the GUI (left button draws, right erases, clear, invert), or load any
  picture (scaled and dithered to 128×32); it is stored in the keyboard's flash. The built-in logo is the default.
- **RAM programs:** upload your own code over USB and run it from RAM (24 KB) without flashing. Programs get the
  keys, pads, knobs, joystick, buttons, LEDs, screen and MIDI out through a small API (`fw/prog_api.h`), can take
  the inputs over from the firmware, or drive the hardware directly. Holding PROG SELECT for 2 s, a crash or a
  replug brings the normal firmware back.
- **Game market** (`mpk3_market.py`): a desktop app that loads games and apps onto the keyboard, unloading the one
  before, with a live picture of the keyboard's screen. Most run on this computer and use the keyboard as their
  screen and controller (a small RAM program, `programs/remote.c`, streams frames in and controls out):
  - **Turbo Loop** — top-down racing: joystick X steers, PAD 8 gas, PAD 4 brake/reverse, three laps.
  - **Cave Glide** — fly through a narrowing cave with knob 1.
  - **Key Catch** — play the key under each falling note.
  - **Beat Lanes** — hit pads 1–4 in time as beats cross the line.
  - **Binary Typer** — opens a terminal window and types into it from the pads in binary (PAD 1 = 0, PAD 2 = 1,
    eight bits per character, PAD 3 deletes, PAD 4 Enter).
  - **Joystick Mouse** — the joystick moves this computer's mouse pointer, PAD 8 left-clicks, PAD 4 right-clicks
    (a virtual mouse via `/dev/uinput`, which your user must be able to write).
  - **Audio Visualiser** — whatever this computer plays, on the keyboard's screen: spectrum (PAD 1), waveform with
    0 dBFS at the edges (PAD 2), waterfall (PAD 3); PAD 4 toggles pitch follow (the waveform shows two cycles of
    the detected note and names it); knob 1 sets sensitivity / zoom.
  - **Fifths** — a RAM program that runs on the keyboard itself (`programs/demo.c`).

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
- For the Audio Visualiser: numpy and PipeWire or PulseAudio (`parec` or `pw-record`)
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

Run a RAM program (`programs/demo.c`: keys play with a fifth on top, pads light by pressure, joystick ball, knob
bars; PROG SELECT returns). Write your own as `programs/<name>.c` with `int program(const prog_api_t *api)`:

```bash
make -C programs
python3 mpk3_cfg.py run programs/demo.bin
```

A program runs in the same chip as the bootloader: one that writes to flash below `0x0800A000` can brick the
keyboard (recovery then needs SWD). Programs that only use the API can't.

The game market, or one game straight from the terminal (needs `make -C programs` first):

```bash
./mpk3_market.py                      # or double-click it in the file manager
python3 -m games race                 # or cave | keycatch | beats | binterm | mouse | visualiser
```

A game is a class with `step(controls, dt)` and `render(screen)` in `games/`, listed in `games/__init__.py`
(`games/cave.py` is a short example). Only one program can hold the keyboard's MIDI port at a time, so close the
settings GUI while the market has something loaded.

## Repository layout

| Path | What |
|------|------|
| `fw/` | Custom firmware (C, no vendor SDK): `board.*` clocks/pins/OLED/LEDs, `usb_midi.*` USB-MIDI driver, `analog.*` pads/knobs/joystick, `ui.*` buttons/LEDs, `menu.*` + `settings.*` on-device menu and flash storage, `seq.*` clock/arpeggiator/note repeat, `cfg.*` SysEx settings protocol, `gfx.*`/`views.*`/`pong.*` drawing, live screens and idle game, `prog.*` + `prog_api.h` RAM programs, `test.c` hardware test firmware |
| `programs/` | RAM programs: start-up code, linker script, a demo and the remote screen (`make -C programs`) |
| `mpk3_market.py`, `games/` | Game market and the games/apps it loads |
| `mpk3_remote.py` | Keyboard as remote screen and controller for programs on this computer |
| `mpk3_termbridge.py` | Shell-in-a-window bridge used by Binary Typer |
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
