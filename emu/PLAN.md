# MPK mini 3 emulator — plan

Goal: an emulator of the Akai MPK mini 3 (AT32 variant) that acts exactly as the real device. Approach: emulate the
chip, not the behaviour — run firmware images instruction by instruction on an emulated AT32F415 (Cortex-M4,
128 KB flash, 32 KB RAM) with models of every peripheral the firmware touches. Analog details (pad pressure curve,
ADC noise, electrical timing) are modelled from `firmware/NOTES.md` and are approximate; everything the CPU does is
exact because it is the same code.

## Definition of done (from the owner)

The work counts as done **only when both of these run in the emulator**:

1. **The official Akai firmware** — the stock image from the owner's local `firmware/stock_v1.26/AT.bin` (fetched
   with `firmware/get_stock_firmware.sh`; never committed or redistributed): it boots through Akai's own bootloader
   region into the stock app, shows the stock screen, enumerates over USB as the stock device, keys/pads/knobs/
   joystick/buttons produce the stock MIDI, and the repo's stock-firmware tools (`mpk3.py`, `mpk3_tui.py`: program
   read/write over SysEx) work against it.
2. **This project's custom firmware** — `fw/mpk.bin` (built with `make -C fw`): boots, home screen and menu work,
   settings save and survive restarts, and `mpk3_cfg.py`, `mpk3_gui.py` and `mpk3_market.py` (including RAM
   programs such as `programs/remote.bin` and Turbo Loop) work against it unchanged.

## Requirements

- Runs a full 128 KB image (bootloader region 0x0000–0x9FFF + app from 0x0800A000) as flashed. Clocks, SysTick
  (custom fw: 20 kHz), DWT CYCCNT (key velocity timing), NVIC, reset (AIRCR) and faults behave as on the chip.
- Flash controller (unlock, page erase, halfword program) backed by an image file: settings and the idle picture
  persist; whatever stock stores in flash persists too.
- Front panel: 128x32 OLED decoded from the bit-banged SSD1306 signals (incl. the page 0–7 behaviour in NOTES),
  LEDs with brightness from their software PWM (active-low shift registers), 25 keys with velocity (two contacts
  per key, timed), 8 pads with pressure, 8 endless knobs, joystick, 10 buttons (incl. PAD BANK + PROG SELECT combos).
- USB as seen from this computer: an ALSA sequencer MIDI port (visible to DAWs) and a raw-MIDI endpoint (PTY) that
  `mpk3_cfg.find_device` / `mpk3.MPK` pick up (e.g. via `MPK3_MIDI`), so the existing tools work unchanged.
- RAM programs run on the emulated CPU (same as hardware).
- Stretch: update mode (PAD BANK + PROG SELECT at power-up) through the stock bootloader's HID protocol so
  `mpk3_flash.py` can flash the emulator.

## Patterns to mirror

| Category | Source | Pattern |
|---|---|---|
| Naming | `mpk3_*.py`, `fw/*.c` | `mpk3_emu*.py` tools; snake_case C under `emu/` with a Makefile like `fw/Makefile` |
| Errors | `mpk3_cfg.py` `Device._status`, `mpk3_market.Market._act` | readable `IOError`/`TimeoutError` messages; GUIs show a dialog + status line |
| Device access | `mpk3.py` `MPK._open`, `mpk3_cfg.find_device` | rawmidi path opened with `os.open`; add the emulator's path first in discovery |
| Tests | `fw/test_keys.c`, `test_*.py` | C host test via `make -C emu check`; Python unittest against a headless emulator |
| Logging | none | terminal prints; unmodelled register accesses logged once each (trace aid) |

## Phases

① first (it fixes the interfaces), then ②③④ in parallel (agents), then ⑤.

1. **CPU + system core** — CPU (recommended: Unicorn engine; the owner installs it — check what Arch names the
   packages, e.g. `unicorn` / `python-unicorn`, and ask before anything needs sudo), memory map, VTOR, NVIC +
   SysTick + exception entry/return (`EXC_RETURN`, priorities, PRIMASK), DWT, RCC/PWC/FLASH_ACR stubs that report
   ready, flash controller → file, AIRCR reset, fault → reset, real-time pacing. Ends with a written interface spec
   for the peripheral models. Test: both images reach their main loop; a custom-fw settings save survives a restart.
2. **GPIO / ADC / DMA / display / LEDs** — key/button matrix (rows driven, IDR GPIOC 8–15 read), mux PB0–2, ADC1
   (custom fw: single conversions; stock: scan of PA0–PA4 into RAM via DMA1 ch1 — model DMA), SSD1306 decoder from
   pin toggles, LED shift registers. Discover stock's extra peripherals (timers, etc.) by tracing unmodelled
   accesses and the stock disassembly notes in `firmware/NOTES.md`. Test: custom home screen matches host-built
   `views.c` pixels; velocity matches `fw/test_keys.c`.
3. **USB** — DWC2 OTG FS device model with the emulator as USB host: enumerate (both firmwares' descriptors —
   stock may be composite), SET_CONFIGURATION, bulk MIDI both ways; bridge to ALSA seq + PTY. Test: `mpk3_cfg.py
   info`/get/set/save on custom fw; `mpk3.py` program read on stock; MIDI clock in, notes out.
4. **Front panel (Python/Tk)** — OLED 4x, LEDs, clickable/typeable keys, pads with pressure, scroll knobs,
   draggable joystick, buttons; talks to the core over a socket; headless mode for tests.
5. **Integration + validation** — `find_device` checks the emulator first; `make -C emu`; `python3 mpk3_emu.py`
   launcher (choose image: custom or local stock); end-to-end runs of every item in the definition of done;
   README section + NOTES; optional update-mode/flasher support.

## Decisions taken

- CPU: Unicorn (the owner installs the packages; ask before anything needs sudo).
- MIDI on the computer: ALSA sequencer + PTY, no root. (`snd-virmidi` would need `sudo modprobe`; optional.)
- Scope: custom + official firmware are both required (definition of done); update mode is the stretch goal.

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Unicorn has no M-profile NVIC/exception model | High | implement exception entry/return in ①; test with SysTick + USB IRQ paths of both images |
| USB core model must match two different USB stacks (custom `usb_midi.c`, Akai's) | High | build against custom first, then trace stock's register sequence; log every unmodelled access |
| Stock uses peripherals the custom fw never touches (DMA, timers, ...) | High | trace-driven discovery; NOTES has stock disassembly findings |
| Per-access hook overhead (bit-banged OLED ~16k pin writes/frame) | Medium | core + peripheral models in C; GUI receives frames and LED levels only |
| Akai code leaking into the repo | Low | load stock only from the local `firmware/stock_v1.26/`; `.gitignore` already blocks it; never commit images |

Complexity: high (≈3–5k lines C + Python).

## House rules for this repo

- Never commit Akai's firmware (`firmware/stock_v1.26/`), built `.bin` images, or `backups/`.
- Commit/push only when the owner asks; commits use the repo's own git identity (ExonSounds noreply).
- Files in the repo with only permission (mode) changes are not ours to commit: stage with
  `git -c core.fileMode=false add <files>`.
- Firmware/host tests: `make -C fw check`; Python: `python3 -m unittest test_mpk3 test_mpk3_cfg test_mpk3_flash
  test_games`.
