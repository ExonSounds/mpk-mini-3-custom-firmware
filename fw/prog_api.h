/* RAM programs: code uploaded over USB (python3 mpk3_cfg.py run prog.bin) into RAM and started by the firmware.
 * Shared by the firmware (prog.c) and programs (programs/). A program is a flat binary linked at PROG_BASE that
 * starts with prog_header_t; the firmware calls entry(api) from its main loop and shows its normal screen again
 * when entry returns. It runs on the firmware's stack with interrupts on, so the keys, pads, knobs, USB and LEDs
 * keep working; call api->poll() often so MIDI and USB commands keep flowing.
 * Programs may also ignore the API and drive the hardware directly (firmware/NOTES.md has the pin map); then they
 * must never write to flash below 0x0800A000 (the stock bootloader).
 * Leaving: return from entry, hold PROG SELECT for 2 s (resets the keyboard), or unplug it. A fault resets it too.
 * Nothing is kept: a reset or replug starts the normal firmware again. */
#pragma once
#include <stdint.h>

#define PROG_BASE 0x20002000u  /* RAM above the firmware's 8 KB (link.ld) */
#define PROG_SIZE 0x6000u      /* 24 KB, up to the end of the 32 KB RAM */
#define PROG_MAGIC 0x50524B4Du /* "MKRP" little-endian */
#define PROG_API_VERSION 2     /* 2: on_message */
#define PROG_MSG_MAX 200       /* bytes in a host message (all < 128) */
#define PROG_REPLY_MAX 64      /* bytes a program may answer with (all < 128) */

typedef struct prog_api prog_api_t;
/* A message from the host (SysEx 0B, mpk3_cfg.Device.message): handle it, write the answer to out, return its
 * length (1..PROG_REPLY_MAX). Called from api->poll(), so it never interrupts the program's own drawing. */
typedef unsigned (*prog_msg_fn)(const uint8_t *in, unsigned n, uint8_t *out);

typedef struct {
    uint32_t magic;                       /* PROG_MAGIC */
    int (*entry)(const prog_api_t *api);  /* return value is ignored */
} prog_header_t;

/* Button bits in *buttons (same as BTN_* in ui.h) */
enum { MPK_BTN_PROG_SELECT, MPK_BTN_CC, MPK_BTN_PROG_CHANGE, MPK_BTN_PAD_BANK, MPK_BTN_ARP, MPK_BTN_TAP_TEMPO,
       MPK_BTN_OCT_UP, MPK_BTN_OCT_DOWN, MPK_BTN_FULL_LEVEL, MPK_BTN_NOTE_REPEAT };
/* LED numbers for led_level[] (as in ui.c); level 0 = off .. 32 = full brightness */
enum { MPK_LED_BANK_A = 0, MPK_LED_BANK_B = 1, MPK_LED_PROG_CHANGE = 2, MPK_LED_CC = 3, MPK_LED_PROG_SELECT = 4,
       MPK_LED_PAD0 = 8 /* pads 1-8: 8..15 */, MPK_LED_ARP = 16, MPK_LED_TAP_TEMPO = 17, MPK_LED_OCT_UP = 18,
       MPK_LED_OCT_DOWN = 19, MPK_LED_FULL_LEVEL = 20, MPK_LED_NOTE_REPEAT = 21 };

struct prog_api {
    uint32_t version;                 /* PROG_API_VERSION */
    const volatile uint32_t *ticks;   /* counts at 20 kHz */
    void (*poll)(void);               /* send queued MIDI, answer USB commands; call at least every few ms */

    /* own_input = 1: the firmware stops acting on keys, pads, knobs, joystick and buttons (no MIDI, no menu, no
     * LED changes) and leaves them to the program; their state below stays live. 0 (start value): normal. */
    volatile uint8_t *own_input;
    void (*keys)(uint8_t vel[25]);    /* 0 = up, else the velocity it was struck with; [0] = lowest key */
    void (*pads)(uint8_t out[8]);     /* 0 = up, 1-127 = live pressure */
    void (*knobs)(uint8_t pos[8], uint8_t value[8], uint8_t *rel);  /* pointer 0-127, sent value, relative mask */
    void (*joystick)(int *x, int *y); /* -1000..1000, right / up positive */
    const volatile uint16_t *buttons; /* held buttons, bit = MPK_BTN_* (debounced) */

    void (*midi_send)(uint32_t pkt);  /* USB-MIDI packet: byte 0 = CIN (e.g. 0x9 note on), 1-3 = MIDI bytes */

    volatile uint8_t *led_level;      /* 24 LEDs, 0..32, software PWM */

    uint8_t *fb;                      /* 128x32 1-bit frame: byte = 8 vertical pixels, fb[(y/8)*128 + x] bit y%8 */
    void (*fb_clear)(void);
    void (*px)(int x, int y);
    void (*rect)(int x, int y, int w, int h);
    int (*text)(int x, int y, const char *s, int scale);  /* 5x7 font, 6*scale px per char; returns end x */
    void (*flush)(void);              /* show fb on the screen (~2 ms) */

    /* version 2 */
    prog_msg_fn *on_message;          /* set *on_message = your handler to get host messages; 0 = none */
};
