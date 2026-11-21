/* Channels, pad note, curves, knob CCs and joystick CCs come from settings (S); defaults = stock program 1:
 * pads notes 36-43 (bank B 44-51), CC 16-31 or program 0-15 on ch 10; knobs CC 1-8 on ch 1; joystick X = pitch
 * bend, Y = CC 1 both ways. */
#include "analog.h"
#include "board.h"
#include "menu.h"
#include "seq.h"
#include "settings.h"
#include "ui.h"

#define PAD_CH  S.pads_ch
#define CTRL_CH S.keys_ch

static inline uint32_t pkt(int cin, int status, int d1, int d2) {
    return (uint32_t)cin | (uint32_t)status << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
}
static inline int iabs(int x) { return x < 0 ? -x : x; }

/* ---- Pads: hit when above 450 (stock threshold), velocity from the peak of the next few samples ---- */
#define PAD_ON   450
#define PAD_OFF  250
#define PAD_MAX  3800  /* shortcut: peak for velocity 127 is a guess; tune with the screen readout */
#define PAD_PEAK 3     /* samples (~1.6 ms apart) to look for the peak after crossing PAD_ON */
enum { P_IDLE, P_RISE, P_ON };
static struct { uint8_t st, n, mode, num; uint16_t peak; } pad[8];  /* mode/num: what the hit sent */

static void pad_hit(int i, int vel) {
    int idx = 8 * ui_bank + i;
    pad[i].mode = ui_pad_mode;
    if (ui_pad_mode == PADS_CC) pad[i].num = 16 + idx, midi_push(pkt(0xB, 0xB0 | PAD_CH, 16 + idx, vel));
    else if (ui_pad_mode == PADS_PROG) pad[i].num = idx, midi_push(pkt(0xC, 0xC0 | PAD_CH, idx, 0));
    else {
        int n = S.pad_note + idx > 127 ? 127 : S.pad_note + idx;
        pad[i].num = n;
        if (seq_repeat_on) pad[i].mode = PADS_REPEAT, repeat_pad(i, n, vel, 1);
        else midi_push(pkt(0x9, 0x90 | PAD_CH, n, vel));
    }
    ui_pad_led(i, 1);
    show3(pad[i].num, vel, pad[i].peak / 10);
}

static void pad_release(int i) {
    if (pad[i].mode == PADS_CC) midi_push(pkt(0xB, 0xB0 | PAD_CH, pad[i].num, 0));
    else if (pad[i].mode == PADS_NOTE) midi_push(pkt(0x8, 0x80 | PAD_CH, pad[i].num, 0x40));
    else if (pad[i].mode == PADS_REPEAT) repeat_pad(i, pad[i].num, 0, 0);
    ui_pad_led(i, 0);
}

void pad_update(int i, uint16_t v) {
    switch (pad[i].st) {
    case P_IDLE:
        if (v >= PAD_ON) pad[i].st = P_RISE, pad[i].peak = v, pad[i].n = 0;
        break;
    case P_RISE:
        if (v > pad[i].peak) pad[i].peak = v;
        if (++pad[i].n >= PAD_PEAK) {
            int p = pad[i].peak > PAD_MAX ? PAD_MAX : pad[i].peak;
            pad_hit(i, ui_full_level ? 127 : curve(1 + (p - PAD_ON) * 126 / (PAD_MAX - PAD_ON), S.pad_curve));
            pad[i].st = P_ON;
        }
        break;
    case P_ON:
        if (v < PAD_OFF) {
            pad_release(i);
            pad[i].st = P_IDLE;
        }
        break;
    }
}

/* ---- Knobs: endless pots with two wipers 90 degrees apart (triangle waves). Use the wiper nearer mid-scale
 * (its linear part); the other wiper's half tells which way that one is sloping. ---- */
/* ADC counts per CC step by KNOB SPEED (slow/normal/fast); normal = ~1.5 turns for 0-127 */
static const int16_t knob_step[3] = {60, 40, 20};
#define KNOB_STEP knob_step[S.knob_speed]
#define KNOB_DEAD 4   /* ignore movement below this (ADC noise) without losing slow turns */
static struct { uint16_t a, b; int16_t acc; uint8_t val, init; } kn[8];

void knob_update(int i, uint16_t a, uint16_t b) {
    if (!kn[i].init) { kn[i].a = a, kn[i].b = b, kn[i].init = 1; return; }
    int da = a - kn[i].a, db = b - kn[i].b, d;
    if (iabs(a - 2048) < iabs(b - 2048)) d = b > 2048 ? da : -da;
    else d = a > 2048 ? -db : db;
    if (iabs(d) < KNOB_DEAD) return;       /* keep the old reference so slow turns still add up */
    kn[i].a = a, kn[i].b = b;
    if (iabs(d) > 1500) return;            /* implausible jump (e.g. first sample after a glitch) */
    kn[i].acc += d;
    int steps = kn[i].acc / KNOB_STEP;
    if (!steps) return;
    kn[i].acc -= steps * KNOB_STEP;
    if (menu_on && i == 0) { menu_adjust(steps); return; }  /* knob 1 edits the menu value */
    int v = kn[i].val + steps;
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    if (v != kn[i].val) {
        kn[i].val = v;
        midi_push(pkt(0xB, 0xB0 | CTRL_CH, S.knob_cc[i], v));
        show3(S.knob_cc[i], v, i + 1);
    }
}

/* ---- Joystick: centre = first reading (stick at rest at power-up); range grows with what is seen ---- */
#define JOY_DEAD 80
#define JOY_HYST 12  /* per mille: at the edges the reading wobbles; v2.00 sent ~100 CC/s holding the stick there */
static struct { uint16_t c, lo, hi; int16_t last, lastr; uint8_t init; } joy[2];

void joy_update(int ax, uint16_t v) {
    if (!joy[ax].init) {
        joy[ax].c = v, joy[ax].lo = v > 800 ? v - 800 : 0, joy[ax].hi = v < 3295 ? v + 800 : 4095;
        joy[ax].last = ax ? 0 : 8192, joy[ax].init = 1;
        return;
    }
    if (v < joy[ax].lo) joy[ax].lo = v;
    if (v > joy[ax].hi) joy[ax].hi = v;
    int d = v - joy[ax].c, r = 0;  /* r: -1000 .. 1000 */
    if (d > JOY_DEAD) r = (d - JOY_DEAD) * 1000 / (joy[ax].hi - joy[ax].c - JOY_DEAD);
    else if (d < -JOY_DEAD) r = (d + JOY_DEAD) * 1000 / (joy[ax].c - joy[ax].lo - JOY_DEAD);
    if (r && iabs(r - joy[ax].lastr) < JOY_HYST) return;  /* centre (r = 0) always gets through */
    joy[ax].lastr = r;
    if (ax == 0 && S.joy_x_cc) {  /* X as a CC: left..right = 0..127 */
        int cc = 64 + r * 63 / 1000;
        if (cc != joy[0].last) {
            joy[0].last = cc;
            midi_push(pkt(0xB, 0xB0 | CTRL_CH, S.joy_x_cc, cc));
            show3(S.joy_x_cc, cc, 0);
        }
    } else if (ax == 0) {  /* X: pitch bend, 14 bit, 8192 = centre */
        int pb = 8192 + r * 8191 / 1000;
        if (pb != joy[0].last) {
            joy[0].last = pb;
            midi_push(pkt(0xE, 0xE0 | CTRL_CH, pb & 0x7F, pb >> 7));
            show3(pb >> 7, pb & 0x7F, 0);
        }
    } else {        /* Y: CC 1 by distance from centre, both directions */
        int cc = iabs(r) * 127 / 1000;
        if (cc != joy[1].last) {
            joy[1].last = cc;
            midi_push(pkt(0xB, 0xB0 | CTRL_CH, S.joy_y_cc, cc));
            show3(S.joy_y_cc, cc, 0);
        }
    }
}

/* ---- Sampling: one conversion per tick. Per mux position: ch0 (knob wiper A), ch1 (wiper B), ch2 (pad),
 * then a joystick axis while the mux settles on the next position. 4 ticks x 8 positions = 1.6 ms per pass. ---- */
static const uint8_t item_at[8] = {1, 2, 3, 0, 7, 4, 6, 5};  /* mux position -> knob/pad index (NOTES.md) */
static uint8_t pos, step, axis;
static uint16_t wa;

void analog_init(void) {
    adc_init();
    mux_select(0);
    delay(1000);
    adc_start(0);
}

void analog_tick(void) {
    if (!adc_done()) return;
    uint16_t v = adc_value();
    int item = item_at[pos];
    switch (step) {
    case 0: wa = v; adc_start(1); break;
    case 1: knob_update(item, wa, v); adc_start(2); break;
    case 2:
        pad_update(item, v);
        pos = (pos + 1) & 7;
        mux_select(pos);
        adc_start(3 + axis);
        break;
    case 3: joy_update(axis, v); axis ^= 1; adc_start(0); break;
    }
    step = (step + 1) & 3;
}
