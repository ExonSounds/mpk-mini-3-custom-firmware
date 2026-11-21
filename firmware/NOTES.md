# MPK mini 3 firmware notes

Source: Akai updater v1.26 (`cdn.inmusicbrands.com/akai/mpk3mini/1_26/`), Mac build (has symbols).
Stock images are in `stock_v1.26/` (AT.bin sha1 2217791f…, STM.bin sha1 a77df789…).

## Hardware variants
| Image | Runtime USB PID | MCU family | Boot region | App vector table |
|-------|-----------------|------------|-------------|------------------|
| AT.bin  | 0x1049 (**this unit**) | Artery AT32 (Cortex-M) | 0x0000–0x4FFF, copy at 0x5000–0x9FFF | 0xA000 (SP 0x20004B70, reset 0x08018A69) |
| STM.bin | 0x0049 | STM32 | 0x0000–0x3FFF, copy at 0x4000–0x7FFF | 0x8000 |

Images are plain (not encrypted, no signature), 128 KB, linked at 0x08000000.

## Board (from disassembling AT.bin app code, not yet checked against the PCB)
**MCU: Artery AT32F415RBT7** (Cortex-M4, 128 KB flash, 32 KB SRAM, LQFP64). Evidence: USB OTGFS at 0x50000000
plus F1-style GPIO/DMA/flash registers at 0x4001xxxx/0x4002xxxx. Of the 128 KB Artery parts, only the F415 has
that combination. All of PC0–PC15 are used, which only the 64-pin package has.
The firmware is built on the STM32F10x StdPeriph library (GPIO_Init at 0x0800C5C8, SetBits 0x0800C6E4,
ResetBits 0x0800C6EC, Write 0x0800C6F4, ReadInputData 0x0800C6DE), so STM32F1-style register code works.

| Function | Pins / peripheral | Notes |
|----------|-------------------|-------|
| Key/button matrix, drive | PC0–PC7, PB8, PB9 (push-pull out) | 10 drive lines, written via GPIOC ODR |
| Key/button matrix, sense | PC8–PC15 (floating in) | 10 × 8 = 80 switch positions (25 keys × 2 velocity contacts + buttons) |
| Analog inputs | PA0–PA4 = ADC1 ch0–4, DMA1 ch1 | through 8:1 analog muxes |
| Analog mux select | PB0, PB1, PB2 (out) | code steps through all 8 combinations; 5 × 8 = 40 inputs (pads, knobs, joystick) |
| OLED (SSD1306-type) | SPI2: PB13 SCK, PB15 MOSI; PB12 CS, PB14 D/C, PB10 RESET; DMA1 ch5 | init: AE 00 10 40 81 .. A6 A4 A8 3F D3 00 D5 80 D9 .. DA 02 DB 20 20 00 A1 C8 8D 14 AF (cmd fn 0x08013250) |
| LEDs (24) | SPI1 remapped: PB3 SCK, PB5 MOSI; PB4 latch, PB7 (likely output enable); DMA1 ch3 | 3 chained 8-bit shift registers; software PWM, bit planes at RAM 0x20003860 (3 bytes per slot), set-brightness fn 0x0800DF4E (LED 0–23) |
| Sustain pedal (probably) | PA5 (floating in) | read in 3 places |
| Unknown output | PA10 | driven high at init |
| USB | OTGFS, PA11/PA12 | |
| SWD | PA13/PA14 | not used as GPIO; remap call 0x02000000 looks like "JTAG off, SWD on", so a debug probe should still work |

Timers TMR1–TMR5 are set up (no timer output pins configured, so they're probably just ticks: scan, PWM, tempo/arp).

## Input/LED mapping (from stock app code; keys, buttons, knobs, pads and joystick confirmed on hardware with fw/ test firmware, 2026-11-21)
### Key/button matrix (scan at 0x0800A3AC, consumer 0x0800A4A8)
One drive line per timer tick, active low: rows 0–7 = PC0–PC7, row 8 = PB8, row 9 = PB9 (all others high).
The sense byte is PC8–PC15 (bit b = PC(8+b)), **0 = closed**. Changes go into a 32-entry event queue at 0x200027A0 with a timestamp.

**Keys:** rows come in pairs (2p, 2p+1). Each key has a first contact on the even row and a second on the odd row;
the time between them gives velocity. Note-on comes from the second contact, note-off from the first contact opening.
`key = 27 − (bit + 8·(row/2))`; key 0 = lowest C (note = octave base + key). Row 0/1 bits 0–2 aren't used (keys 25–27).

| Key | Note | 1st contact row | 2nd contact row | Sense |
|-----|------|-----------------|-----------------|-------|
| 0 | C | PC6 | PC7 | PC11 |
| 1 | C# | PC6 | PC7 | PC10 |
| 2 | D | PC6 | PC7 | PC9 |
| 3 | D# | PC6 | PC7 | PC8 |
| 4 | E | PC4 | PC5 | PC15 |
| 5 | F | PC4 | PC5 | PC14 |
| 6 | F# | PC4 | PC5 | PC13 |
| 7 | G | PC4 | PC5 | PC12 |
| 8 | G# | PC4 | PC5 | PC11 |
| 9 | A | PC4 | PC5 | PC10 |
| 10 | A# | PC4 | PC5 | PC9 |
| 11 | B | PC4 | PC5 | PC8 |
| 12 | C+1 | PC2 | PC3 | PC15 |
| 13 | C#+1 | PC2 | PC3 | PC14 |
| 14 | D+1 | PC2 | PC3 | PC13 |
| 15 | D#+1 | PC2 | PC3 | PC12 |
| 16 | E+1 | PC2 | PC3 | PC11 |
| 17 | F+1 | PC2 | PC3 | PC10 |
| 18 | F#+1 | PC2 | PC3 | PC9 |
| 19 | G+1 | PC2 | PC3 | PC8 |
| 20 | G#+1 | PC0 | PC1 | PC15 |
| 21 | A+1 | PC0 | PC1 | PC14 |
| 22 | A#+1 | PC0 | PC1 | PC13 |
| 23 | B+1 | PC0 | PC1 | PC12 |
| 24 | C+2 | PC0 | PC1 | PC11 |

**Buttons:** `button = bit + 8·(row − 8)`. Press is accepted after a debounce; long-hold events follow.
Names come from what each case does in the press handler 0x0800EF76 (strings, octave math, pad-mode switch).
LEDs come from the factory-test table in RAM 0x20000058 (4 bytes per button: state, test note, LED A, LED B; 0x18 = none).

| # | Button | Drive | Sense | LED(s) | Evidence |
|---|--------|-------|-------|--------|----------|
| 0 | PROG SELECT | PB8 | PC8 | 4 | lights LED 4 + current program's pad LED |
| 1 | CC | PB8 | PC9 | 3 | toggles pad mode 1 |
| 2 | PROG CHANGE | PB8 | PC10 | 2 | toggles pad mode 2 |
| 3 | PAD BANK | PB8 | PC11 | 0 = bank A, 1 = bank B | toggle fn 0x08011824: bank 0 → LED 0, bank 1 → LED 1 (both under the same button) |
| 4 | ARP | PB8 | PC12 | 16 | menu shows "ARP Off / LAT / OCT / 50%" |
| 5 | TAP TEMPO | PB8 | PC13 | 17 | "BPM" |
| 6 | OCT+ | PB8 | PC14 | 18 | octave++ (0–8, default 4); 6+7 together = special |
| 7 | OCT− | PB8 | PC15 | 19 | octave−− |
| 8 | FULL LEVEL | PB9 | PC8 | 20 | the only one left (no strings) |
| 9 | NOTE REPEAT | PB9 | PC9 | 21 | "NOTE REPEAT" |
Update mode = BANK (3) + PROG SELECT (0) held at power-up.

### Analog (ADC1 scan of PA0–PA4 into RAM 0x200039E0 via DMA1 ch1; mux select PB0 = bit0, PB1 = bit1, PB2 = bit2)
After each scan the mux moves to the next position (0–7), and readings go into a table at 0x20003318 (8 bytes per entry, value first).
Entry = `channel·8 + mux position` for PA0–PA2 (0–23); entry 24 = PA3, entry 25 = PA4.
For each mux, the item i → mux position order is the same: `[3, 0, 1, 2, 5, 7, 6, 4]` (tables at 0x08018D4C / D54 / DFC).

| Input | Source | Evidence |
|-------|--------|----------|
| Knob i (1–8), wiper A | PA0, mux position `[3,0,1,2,5,7,6,4][i−1]` | read in pairs with wiper B; endless pots (two wipers 90° apart) |
| Knob i (1–8), wiper B | PA1, same mux position | |
| Pad i (1–8) | PA2, same mux position | hit threshold 450; pad i lights LED 8+i−1; pad mode/bank logic (0x080111A0) |
| Joystick X | PA3 (not muxed) | axis 0 uses program byte 31 ("X mode"); auto-calibrated min/max scaled to 0–4095 |
| Joystick Y | PA4 (not muxed) | axis 1 uses program byte 34 ("Y mode") |
Knob 1 / Pad 1 / joystick axes confirmed on hardware; whether wiper A/B is sin or cos is unknown.

### LEDs (index for set-brightness 0x0800DF4E)
| Index | LED |
|-------|-----|
| 0 | PAD BANK, bank A |
| 1 | PAD BANK, bank B |
| 2 | PROG CHANGE |
| 3 | CC |
| 4 | PROG SELECT |
| 5–7 | not referenced by the button table (unknown/unused) |
| 8–15 | Pads 1–8 |
| 16 | ARP |
| 17 | TAP TEMPO |
| 18 | OCT+ |
| 19 | OCT− |
| 20 | FULL LEVEL |
| 21 | NOTE REPEAT |
| 22, 23 | unknown/unused |
LEDs 0, 1 and 8–15 get full PWM range; the rest are capped at brightness 4 (probably different LED types).
**The shift register outputs are active low: bit 0 = LED on.** Stock's set-brightness clears the bit for slots
< brightness and sets it for the rest. LED 0 / 1 are the red and green halves of the PAD BANK light.

### Custom firmware (fw/)
- Build: `make -C fw` (clang + ld.lld) → `fw/mpk.bin` (custom firmware) and `fw/test.bin` (hardware test);
  `make -C fw check` runs the host test of the key logic. Flash with `python3 mpk3_flash.py fw/mpk.bin`.
- Stage 1 (v1.00), tested 2026-11-21: enumerates as class-compliant USB-MIDI `1209:0001 "MPK mini 3 custom"`;
  keys send note on/off with velocity (ch 1, lowest key = 48). Soft→hard presses gave velocities ~30→127.
  Clock and USB core setup copied from stock (HSE 8 MHz ×6, USB = PLL/1, GCCFG 0x002D0000, IRQ 67).
- Stage 2 (v2.00), tested 2026-11-21: pads (notes 36–43, ch 10, velocity 8–127 seen), knobs (CC 1–8 absolute,
  smooth single steps), joystick X pitch bend (full 1–16383, returns to 8192), Y → CC 1 both ways.
  The joystick edge wobble flooded CC 1 (~100/s); v2.01 adds 1% hysteresis (tested: holding at the edge = 1 message).
  Confirmed: knob clockwise = value up (with the wiper-sign convention in analog.c), joystick right = pitch up.
- Stage 3 (v3.01), tested 2026-11-21: OCT+/OCT− (±4, both = reset), PAD BANK (A/B, notes 44–51), FULL LEVEL,
  CC (16–31) and PROG CHANGE (0–15) pad modes, LEDs via software PWM (buttons 4/32, bank and pads full).
  v3.00 had the LED polarity backwards (see LEDs above).
- Stage 4 (v4.00), tested 2026-11-21: on-device menu (PROG SELECT open/close+save, OCT−/OCT+ select, knob 1 edits;
  18 settings: channels, transpose, velocity curves, pad note, knob CCs/speed, joystick CCs, reset). Settings live in
  flash at 0x0801E000 (magic "MPK1" + struct + sum16), survive a power cycle (keys ch 3 kept), and are wiped by any
  firmware flash.
- Stage 5 (v5.00), tested 2026-11-21: internal 96 PPQN clock; ARP (6 modes, 1–4 octaves, rate, latch) cycles a held
  chord in time; NOTE REPEAT retriggers held pads evenly; TAP TEMPO (avg of last 3 taps, 40–240 BPM, not saved until
  the menu is closed); screen fully off after SCREEN OFF idle time (default 10 s), any input wakes it.
  Pad numbering is Akai's: bottom row 1–4, top row 5–8.
- v6.00, tested 2026-11-21: FULL LEVEL also forces keys to velocity 127. CLOCK = EXTERNAL follows MIDI clock
  (F8 = 4 pulses; each F8 sets the count to exactly 4×clocks, up to 3 pulses interpolated in between) and
  Start/Continue/Stop from the host (EP1 OUT, CIN 0xF). With 100 BPM clock sent from the PC: median step 150 ms
  (1/16), notes on average 0.2 ms from a clock, silent after Stop.
- v7.00, tested 2026-11-21: settings over SysEx (fw/cfg.h): `F0 7D 4D 4B cmd … F7` (7D = non-commercial ID);
  HELLO / GET / SET / SAVE / DEFAULTS, settings as nibbles (tempo > 127). Reassembled from USB-MIDI CIN 4–7 in the
  USB interrupt, run from the main loop (SAVE writes flash); MIDI queue 64 → 256 packets. Checked from the PC with
  mpk3_cfg.py: read-back exact, SAVE verified, invalid SET rejected by the firmware (status 2).
- v8.00 (settings layout 4, 104 bytes), tested 2026-11-21: per-pad note/CC/program (16), per-knob CC/min/max/
  relative, joystick mode per axis (pitch / one CC / CC per direction), knob+joystick channel, pad aftertouch
  (off/channel/poly), arp swing 50–75 % and gate 5–100 %. On the keyboard: knob range 20–40 kept, joystick down/up
  on CC 30/31, channel aftertouch 0–117, pad A3 custom note 60, relative knob 63/65. Struct copies need
  memcpy/memset (provided in board.c; no libc).
- The bootloader jumps to `[0x0800A004]` with interrupts disabled and without loading SP, so the app must set SP and VTOR itself.
- OLED: frames pinned to pages 0–3 are invisible. The glass shows the other half of the controller's 64-row memory,
  or which half is visible varies. Writing the frame to pages 0–7 (twice) works. Stock avoids the problem by letting frames alternate.

### Other findings
- RAM init data is IAR LZ77-compressed at 0x08018F38 (0x1AD bytes → 0x20000000); the decoder logic is at 0x08018940.
- Hidden factory test mode (flag 0x20003B4B): keys send note key+30, buttons send note 9+n and toggle their LEDs; strings include
  "Device Configuration", "Press any button", "Press a pad", "JOYSTICK", "calibration", "Snake Game". How to enter it is not found yet.

## Image trailer (last 16 bytes)
- 0x1FFF0: VID, big-endian (09 E8)
- 0x1FFF2: PID, big-endian (10 49)
- 0x1FFF6: version, 2 bytes (01 26)
- 0x1FFFE: checksum = `sum(img[0:0x1FFFE]) & 0xFFFF`, big-endian. Verified on both images.

The updater only checks VID/PID/checksum before flashing.

## Update protocol (USB HID, 32-byte reports, report ID 0)
Enter update mode: hold BANK + PROG SELECT while plugging in USB.
Host to device = `55 4F` ("UO") + cmd. Device to host = `55 49` ("UI") + status:
**`F<n>` = success, `E<n>` = failure** (traced through the updater's branches for all four commands).

| Cmd | Meaning | OK reply |
|-----|---------|----------|
| C4 | connect / get version (what "Connect" sends) | UI F4 <ver hi> <ver lo> |
| C1 | erase (takes effect immediately) | UI F1 |
| C2 | write 16 data bytes | UI F2 |
| C3 | end | UI F3 |

Updater order: C4 (connect), C1 (erase), C4 (version), C2 × 8192, C3.
In update mode the keyboard keeps the same VID:PID 09e8:1049, the same HID descriptor
(vendor page 0xFFA0, 32-byte in/out reports) and even its USB-MIDI port, so USB alone can't tell the modes apart.

Tested on hardware, 2026-11-21: `mpk3_flash.py` flashed stock v1.26 in 8.8 s (every packet acknowledged).
After a normal replug the firmware ran, and all 9 stored programs were unchanged, so program memory is not wiped by the erase.

C2 packet layout:
- [0..2] 55 4F C2
- [3..6] flash address, big-endian (mcuStartAddr + offset; mcuStartAddr = 0)
- [7..8] packet counter, big-endian, starting at 0
- [9..24] 16 data bytes
- [25..29] 0
- [30..31] sum16 of the 16 data bytes, big-endian

Each command is retried up to 3 times with a 3000 ms timeout (from the XML).
No read-back command exists, so flash can't be dumped over USB (we don't need to: the stock image is above).

## Open questions
- Does the bootloader skip its own region (0x0000–0x9FFF), or does it really rewrite it? Until we know, custom
  images must keep 0x0000–0x9FFF byte-identical to stock and change only the app area 0xA000–0x1FFEF.
- How the running (non-update) firmware answers C4. Not tested: it might reboot into the bootloader.
