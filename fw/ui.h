/* Buttons (matrix rows 8/9) and LEDs. Button and LED numbers: firmware/NOTES.md. */
#pragma once
#include <stdint.h>

enum { BTN_PROG_SELECT, BTN_CC, BTN_PROG_CHANGE, BTN_PAD_BANK, BTN_ARP, BTN_TAP_TEMPO, BTN_OCT_UP, BTN_OCT_DOWN,
       BTN_FULL_LEVEL, BTN_NOTE_REPEAT };
enum { PADS_NOTE, PADS_CC, PADS_PROG, PADS_REPEAT };  /* PADS_REPEAT: per-hit only (note mode + NOTE REPEAT) */

extern volatile int8_t ui_octave;       /* -4 .. +4 */
extern volatile uint8_t ui_bank;        /* 0 = A, 1 = B */
extern volatile uint8_t ui_pad_mode;    /* PADS_* */
extern volatile uint8_t ui_full_level;

void ui_button_row(int row, uint8_t closed);  /* from SysTick, rows 8 and 9 */
void ui_led_tick(void);                       /* from SysTick (20 kHz): LED software PWM */
void ui_pad_led(int pad, int on);
extern volatile uint8_t ui_leds_off;          /* 1 = all LEDs dark (idle screen OFF) */
void ui_activity(void);                       /* something was used: keeps the screen on (main.c) */
