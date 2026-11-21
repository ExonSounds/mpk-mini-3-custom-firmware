/* Pads, knobs and joystick: sampled one ADC conversion per SysTick tick, turned into MIDI via midi_push(). */
#pragma once
#include <stdint.h>

void analog_init(void);
void analog_tick(void);  /* call every SysTick (20 kHz) */

/* Per-sample logic, exposed for the host test (make -C fw check). */
void pad_update(int pad, uint16_t v);
void knob_update(int knob, uint16_t a, uint16_t b);
void joy_update(int axis, uint16_t v);

/* Provided by main.c */
void midi_push(uint32_t pkt);
void show3(int a, int b, int c);
