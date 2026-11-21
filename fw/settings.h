/* User settings, edited in the on-device menu and kept in flash at SETTINGS_ADDR. */
#pragma once
#include <stdint.h>

#define SETTINGS_ADDR 0x0801E000u  /* own flash sector(s), below the bootloader-checked trailer at 0x0801FFF0 */

enum { CURVE_SOFT, CURVE_NORMAL, CURVE_HARD, CURVE_FIXED };

typedef struct {
    uint8_t keys_ch, pads_ch;    /* 0-15 (shown 1-16); knobs and joystick use keys_ch */
    uint8_t transpose;           /* 0-24, 12 = none */
    uint8_t key_curve, pad_curve;
    uint8_t pad_note;            /* bank A pad 1; bank B adds 8 */
    uint8_t knob_cc[8];
    uint8_t knob_speed;          /* 0 slow, 1 normal, 2 fast */
    uint8_t joy_x_cc;            /* 0 = pitch bend */
    uint8_t joy_y_cc;
    uint8_t arp_mode;            /* ARP_* (seq.h) */
    uint8_t arp_div;             /* 0-7: 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T */
    uint8_t arp_oct;             /* 0-3 (shown 1-4) */
    uint8_t arp_latch;
    uint8_t tempo;               /* 40-240 BPM */
    uint8_t screen_off;          /* 0-4: 5 s, 10 s, 30 s, 60 s, never */
    uint8_t clock_ext;           /* 0 internal tempo, 1 follow MIDI clock from USB */
} settings_t;

extern settings_t S;

void settings_defaults(void);
void settings_load(void);   /* defaults if the flash record is missing or corrupt */
int settings_save(void);    /* 1 = written and verified; call with nothing else touching flash */
int curve(int vel, int which);
