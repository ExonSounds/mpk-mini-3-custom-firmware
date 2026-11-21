/* Pads, knobs and joystick: sampled one ADC conversion per SysTick tick, turned into MIDI via midi_push(). */
#pragma once
#include <stdint.h>

void analog_init(void);
void analog_tick(void);  /* call every SysTick (20 kHz) */

/* Per-sample logic, exposed for the host test (make -C fw check). */
void pad_update(int pad, uint16_t v);
void knob_update(int knob, uint16_t a, uint16_t b);
void joy_update(int axis, uint16_t v);

/* For the live screens (views.c) */
void pads_level(uint8_t out[8]);                                 /* 0 = up, 1-127 = held, by live pressure */
void knob_state(uint8_t pos[8], uint8_t value[8], uint8_t *rel); /* pointer position, sent value, relative mask */
void joy_state(int *x, int *y);                                  /* -1000..1000, right / up positive */

/* Provided by main.c */
void midi_push(uint32_t pkt);
enum { V_HOME, V_KEYS, V_PADS, V_KNOBS, V_JOY, V_MSG, V_TEMPO };
void show_view(int v);              /* switch the screen to a view and redraw it (MSG/TEMPO time out to HOME) */
void show_msg(const char *title, const char *value);  /* a button's new state, ~1.5 s */
