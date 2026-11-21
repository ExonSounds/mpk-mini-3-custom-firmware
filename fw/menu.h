/* On-device settings menu: PROG SELECT opens/closes (closing saves), OCT-/OCT+ pick the setting, knob 1 changes it. */
#pragma once
#include <stdint.h>

extern volatile uint8_t menu_on, menu_dirty, save_req;

void menu_toggle(void);
void menu_move(int d);
void menu_adjust(int d);
void menu_draw(void);
