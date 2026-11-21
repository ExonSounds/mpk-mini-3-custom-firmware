/* Live screens while playing: the keyboard with held keys, the pad grid with held pads, all 8 dials. */
#pragma once
#include <stdint.h>

void draw_keys(const uint8_t level[25]);          /* key k (0 = lowest C): 0 = up, 1-127 = held (fill density) */
void draw_pads(const uint8_t level[8], int bank); /* pad i+1 (Akai numbering: bottom row 1-4): 0 = up, 1-127 */
void draw_knobs(const uint8_t pos[8], const uint8_t value[8], uint8_t rel);  /* pos 0-127 sets the pointer */
void draw_joystick(int x, int y);                  /* -1000..1000 each; right and up are positive */

typedef struct {
    int8_t octave;                     /* -4..4 */
    uint8_t bank, pad_mode, arp, repeat, full, ext, usb;
    uint8_t bpm;                       /* 0 = unknown (external clock not running) */
} home_t;
void draw_home(const home_t *h);                   /* status overview: startup and when nothing else is shown */
void draw_msg(const char *title, const char *value);  /* a button's new state, menu style */
void draw_tempo(int bpm, int ext, int beat);       /* tap tempo / external clock BPM, beat dot */
void draw_idle_image(const uint8_t *img, int invert);  /* img = 512 bytes in fb layout, or 0 for the built-in logo */
