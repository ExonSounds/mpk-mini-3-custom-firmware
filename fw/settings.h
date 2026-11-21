/* User settings, edited in the on-device menu and kept in flash at SETTINGS_ADDR. */
#pragma once
#include <stdint.h>

#define SETTINGS_ADDR 0x0801E000u  /* own flash sector(s), below the bootloader-checked trailer at 0x0801FFF0 */
#define SETTINGS_LAYOUT 4          /* bump when settings_t changes; also in the flash magic and the GUI */

enum { CURVE_SOFT, CURVE_NORMAL, CURVE_HARD, CURVE_FIXED };
enum { JOY_PITCH, JOY_CC, JOY_DUAL };  /* pitch bend / one CC over the whole range / one CC per direction */

typedef struct {
    /* single settings: one limit each in settings.c lim[], in this order (test_mpk3_cfg.py checks it) */
    uint8_t keys_ch, pads_ch, ctrl_ch;   /* 0-15 (shown 1-16); ctrl = knobs and joystick */
    uint8_t transpose;                   /* 0-24, 12 = none */
    uint8_t key_curve, pad_curve;        /* CURVE_* */
    uint8_t pad_at;                      /* pad aftertouch: 0 off, 1 channel, 2 poly */
    uint8_t knob_speed;                  /* 0 slow, 1 normal, 2 fast */
    uint8_t joy_x_mode, joy_x_neg, joy_x_pos;  /* JOY_*; CC for left / right (single CC mode uses _pos) */
    uint8_t joy_y_mode, joy_y_neg, joy_y_pos;  /* same for down / up */
    uint8_t arp_mode;                    /* ARP_* (seq.h) */
    uint8_t arp_div;                     /* 0-7: 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T */
    uint8_t arp_oct;                     /* 0-3 (shown 1-4) */
    uint8_t arp_latch;
    uint8_t tempo;                       /* 40-240 BPM */
    uint8_t swing;                       /* 50-75 %: delays every second step */
    uint8_t gate;                        /* 5-100 % of a step */
    uint8_t screen_off;                  /* 0-4: 5 s, 10 s, 30 s, 60 s, never */
    uint8_t clock_ext;                   /* 0 internal tempo, 1 follow MIDI clock from USB */
    /* per knob / per pad (bank A = 0-7, bank B = 8-15) */
    uint8_t knob_cc[8], knob_min[8], knob_max[8];
    uint8_t knob_rel[8];                 /* 0 absolute, 1 relative (64 +/- steps) */
    uint8_t pad_note[16], pad_cc[16], pad_pc[16];
    uint8_t idle_screen;                 /* 0 ping-pong, 1 screen off; was a reserved 0 byte, so old saves = pong */
} settings_t;

extern settings_t S;

void settings_defaults(void);
void settings_load(void);   /* defaults if the flash record is missing or corrupt */
int settings_save(void);    /* 1 = written and verified; call with nothing else touching flash */
int curve(int vel, int which);
int settings_valid(const settings_t *s);
