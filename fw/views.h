/* Live screens while playing: the keyboard with held keys, the pad grid with held pads, all 8 dials. */
#pragma once
#include <stdint.h>

void draw_keys(const uint8_t level[25]);          /* key k (0 = lowest C): 0 = up, 1-127 = held (fill density) */
void draw_pads(const uint8_t level[8], int bank); /* pad i+1 (Akai numbering: bottom row 1-4): 0 = up, 1-127 */
void draw_knobs(const uint8_t pos[8], const uint8_t value[8], uint8_t rel);  /* pos 0-127 sets the pointer */
void draw_joystick(int x, int y);                  /* -1000..1000 each; right and up are positive */
