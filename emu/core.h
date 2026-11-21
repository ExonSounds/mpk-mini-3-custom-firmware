/* MPK mini 3 emulator: interface between the CPU/system core (core.c) and the peripheral models.
 *
 * The core runs an AT32F415RB (Cortex-M4, no FPU, 128 KB flash, 32 KB SRAM) on Unicorn, one emulator thread.
 * Everything below is called from that thread only (CPU hooks, MMIO callbacks, events), except where a function
 * says "any thread". Peripheral models follow this pattern:
 *
 *   static uint32_t my_rd(void *ctx, uint32_t off, int size) { ... }        // off = address - base
 *   static void my_wr(void *ctx, uint32_t off, uint32_t val, int size) { ... }
 *   static void my_reset(void *ctx) { ... }                                  // chip reset (power-up, AIRCR)
 *   void my_init(void) { mmio_add("TMR3", 0x40000400, 0x400, my_rd, my_wr, 0); on_reset(my_reset, 0); }
 *
 * and get called from main.c's init list. Time is in CPU cycles (F_CPU per second, 1 instruction = 1 cycle);
 * use ev_at() for anything that happens later (conversion done, timer update, USB frame), never wall-clock time.
 *
 * Address windows the core maps as MMIO (accesses outside registered ranges are logged once per address and
 * behave as plain storage): 0x40000000-0x4002FFFF (APB1/APB2/AHB), 0x50000000-0x5003FFFF (USB OTG FS),
 * 0xE0000000-0xE00FFFFF (PPB: the core owns NVIC/SCB/SysTick/DWT; others may register, e.g. DBGMCU).
 * Bit-band aliases (0x22000000, 0x42000000) are translated to read-modify-write of the target word.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <unicorn/unicorn.h>

#define F_CPU 48000000ull
#define FLASH_BASE 0x08000000u
#define FLASH_SIZE 0x20000u
#define SRAM_BASE 0x20000000u
#define SRAM_SIZE 0x8000u

extern uc_engine *uc;
extern uint8_t *flash_mem;  /* FLASH_SIZE bytes, mmap of the flash state file (writes persist) */
extern uint8_t *sysmem;     /* 4 KB at 0x1FFFF000 (option bytes at +0x800), stored after the flash in that file */
#define STATE_SIZE (FLASH_SIZE + 0x1000u)
extern uint8_t sram[SRAM_SIZE];
extern int emu_verbose;     /* -v: log every unmodelled access, not just the first per address */

/* ---- time ---- */
uint64_t now(void);                       /* cycles since power-up (start of the current block + stalls) */
void stall(uint64_t cycles);              /* CPU blocked for this long (e.g. flash erase): time jumps ahead */
static inline uint64_t us_to_cycles(double us) { return (uint64_t)(us * (F_CPU / 1000000.0)); }

/* ---- events: fn(ctx) runs once at cycle `when` (between instructions). Re-arm from inside fn for periodic. */
typedef void (*ev_fn)(void *ctx);
void ev_at(uint64_t when, ev_fn fn, void *ctx);  /* replaces a pending event with the same fn+ctx */
void ev_cancel(ev_fn fn, void *ctx);

/* ---- MMIO ---- */
typedef uint32_t (*rd_fn)(void *ctx, uint32_t off, int size);
typedef void (*wr_fn)(void *ctx, uint32_t off, uint32_t val, int size);
void mmio_add(const char *name, uint32_t base, uint32_t size, rd_fn rd, wr_fn wr, void *ctx);
void mmio_unmodelled(const char *what, uint32_t addr, int is_write, uint32_t val);  /* log once per address */
uint32_t bus_read(uint32_t addr, int size);              /* as a DMA master: registers via their models */
void bus_write(uint32_t addr, uint32_t val, int size);

/* ---- interrupts (irq = external IRQ number: 0 = WWDT ... 29 = TMR3, 67 = OTGFS) ---- */
void irq_level(int irq, int level);   /* level-sensitive line: pending while high (re-pends after the ISR) */
void irq_pulse(int irq);              /* edge: set pending once */

/* ---- reset ---- */
void on_reset(void (*fn)(void *ctx), void *ctx);  /* called on every chip reset, in registration order */
void request_reset(const char *why);              /* AIRCR, watchdog, panel power cycle: takes effect next stop */

/* ---- cross-thread work: fn(ctx) runs on the emulator thread at the next stop (any thread may call) ---- */
void post(void (*fn)(void *ctx), void *ctx);

/* ---- GPIO (gpio.c): ports 0 = A, 1 = B, 2 = C, 3 = D (F1-style registers at 0x40010800 + 0x400 * port) ---- */
uint16_t gpio_odr(int port);                       /* output data register as written */
uint16_t gpio_out(int port);                       /* pin levels: ODR on output pins, 1 on input pins */
typedef void (*gpio_watch_fn)(void *ctx, int port, uint16_t old_levels, uint16_t new_levels);
void gpio_watch(int port, gpio_watch_fn fn, void *ctx);   /* called after every change of gpio_out(port) */
typedef uint16_t (*gpio_input_fn)(void *ctx, int port);
void gpio_input(int port, gpio_input_fn fn, void *ctx);   /* supplies IDR bits for input pins (sampled on read) */

/* ---- front panel (panel_srv.c; socket protocol at the top of that file) ----
 * Inputs arrive on the emulator thread as calls to these, implemented by the input models (inputs.c): */
void input_key(int key, int velocity);   /* key 0-24, velocity 1-127, 0 = release */
void input_button(int button, int down); /* 0-9, numbering of firmware/NOTES.md */
void input_pad(int pad, int force);      /* pad 0-7 = pads 1-8, force 0-127 (0 = released) */
void input_knob(int knob, int degrees);  /* knob 0-7 turned by degrees, + = clockwise */
void input_joystick(int x, int y);       /* -1000..1000, + = right / up */
void input_pedal(int down);
/* Outputs, called by the display/LED models whenever what a person would see changes (duplicates are dropped): */
void panel_frame(const uint8_t fb[512], int on, int contrast, int invert);  /* visible 128x32, SSD1306 page order */
void panel_leds(const uint8_t level[24]);                                   /* perceived brightness 0-255 */
void panel_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* any thread */

extern const char *emu_run_dir;  /* per-user runtime directory ($XDG_RUNTIME_DIR/mpk3-emu): sockets, MIDI PTY link */

/* ---- logging ---- */
#define LOG(...) (fprintf(stderr, "[emu] " __VA_ARGS__), fputc('\n', stderr))
