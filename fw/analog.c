/* Everything here follows the settings (S): per-pad note/CC/program, per-knob CC and range or relative mode,
 * joystick mode and CCs per axis, pad aftertouch. Defaults = stock program 1 (settings.c). */
#include "analog.h"
#include "board.h"
#include "menu.h"
#include "seq.h"
#include "settings.h"
#include "ui.h"

#define PAD_CH  S.pads_ch
#define CTRL_CH S.ctrl_ch

static inline uint32_t pkt(int cin, int status, int d1, int d2) {
    return (uint32_t)cin | (uint32_t)status << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
}
static inline int iabs(int x) { return x < 0 ? -x : x; }
static inline int clamp127(int v) { return v < 0 ? 0 : v > 127 ? 127 : v; }

/* ---- Pads: hit when above 450 (stock threshold), velocity from the peak of the next few samples ---- */
#define PAD_ON   450
#define PAD_OFF  250
#define PAD_MAX  3800  /* shortcut: peak for velocity 127 is a guess; tune with the screen readout */
#define PAD_PEAK 3     /* samples (~1.6 ms apart) to look for the peak after crossing PAD_ON */
enum { P_IDLE, P_RISE, P_ON };
static struct { uint8_t st, n, mode, num, at, press; uint16_t peak; } pad[8];  /* mode/num: what the hit sent */
static uint8_t chan_at;  /* last channel pressure sent */

static int pressure_pad(int i) { return pad[i].st == P_ON && (pad[i].mode == PADS_NOTE || pad[i].mode == PADS_REPEAT); }

static void send_at(int i) {
    if (S.pad_at == 2) {
        midi_push(pkt(0xA, 0xA0 | PAD_CH, pad[i].num, pad[i].at));
        return;
    }
    int m = 0;  /* channel pressure: the firmest held pad */
    for (int j = 0; j < 8; j++) if (pressure_pad(j) && pad[j].at > m) m = pad[j].at;
    if (m != chan_at) chan_at = m, midi_push(pkt(0xD, 0xD0 | PAD_CH, m, 0));
}

static void pad_hit(int i, int vel) {
    int idx = 8 * ui_bank + i;
    pad[i].mode = ui_pad_mode, pad[i].at = 0;
    if (ui_pad_mode == PADS_CC) pad[i].num = S.pad_cc[idx], midi_push(pkt(0xB, 0xB0 | PAD_CH, pad[i].num, vel));
    else if (ui_pad_mode == PADS_PROG) pad[i].num = S.pad_pc[idx], midi_push(pkt(0xC, 0xC0 | PAD_CH, pad[i].num, 0));
    else {
        pad[i].num = S.pad_note[idx];
        if (seq_repeat_on) pad[i].mode = PADS_REPEAT, repeat_pad(i, pad[i].num, vel, 1);
        else midi_push(pkt(0x9, 0x90 | PAD_CH, pad[i].num, vel));
    }
    ui_pad_led(i, 1);
    show_view(V_PADS);
}

static void pad_release(int i) {
    if (S.pad_at && pressure_pad(i) && pad[i].at) pad[i].at = 0, send_at(i);
    if (pad[i].mode == PADS_CC) midi_push(pkt(0xB, 0xB0 | PAD_CH, pad[i].num, 0));
    else if (pad[i].mode == PADS_NOTE) midi_push(pkt(0x8, 0x80 | PAD_CH, pad[i].num, 0x40));
    else if (pad[i].mode == PADS_REPEAT) repeat_pad(i, pad[i].num, 0, 0);
    ui_pad_led(i, 0);
    show_view(V_PADS);
}

static void pad_pressure(int i, uint16_t v) {  /* aftertouch while held: PAD_OFF..PAD_MAX -> 0..127 */
    if (!S.pad_at || !pressure_pad(i)) return;
    int p = clamp127((v - PAD_OFF) * 127 / (PAD_MAX - PAD_OFF));
    if (iabs(p - pad[i].at) < 2 && p != 0 && p != 127) return;  /* ignore ADC noise */
    if (p == pad[i].at) return;
    pad[i].at = p;
    send_at(i);
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
            pad[i].st = P_ON;
            pad[i].press = clamp127((p - PAD_OFF) * 127 / (PAD_MAX - PAD_OFF));
            pad_hit(i, ui_full_level ? 127 : curve(1 + (p - PAD_ON) * 126 / (PAD_MAX - PAD_ON), S.pad_curve));
        }
        break;
    case P_ON:
        if (v < PAD_OFF) {
            pad_release(i);
            pad[i].st = P_IDLE;
        } else {
            int p = clamp127((v - PAD_OFF) * 127 / (PAD_MAX - PAD_OFF));
            if (p >> 3 != pad[i].press >> 3) show_view(V_PADS);  /* redraw when the dither level changes */
            pad[i].press = p;
            pad_pressure(i, v);
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
static struct { uint16_t a, b; int16_t acc; uint8_t val, out, init; } kn[8];  /* val: 0-127 position, out: last sent */

void knob_update(int i, uint16_t a, uint16_t b) {
    if (!kn[i].init) { kn[i].a = a, kn[i].b = b, kn[i].out = 255, kn[i].init = 1; return; }
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
    if (S.knob_rel[i]) {                   /* relative: 64 + steps (65 = one step up, 63 = one down) */
        int v = 64 + (steps > 63 ? 63 : steps < -63 ? -63 : steps);
        midi_push(pkt(0xB, 0xB0 | CTRL_CH, S.knob_cc[i], v));
        show_view(V_KNOBS);
        return;
    }
    kn[i].val = clamp127(kn[i].val + steps);
    int out = S.knob_min[i] + kn[i].val * (S.knob_max[i] - S.knob_min[i]) / 127;  /* min > max = reversed */
    if (out != kn[i].out) {
        kn[i].out = out;
        midi_push(pkt(0xB, 0xB0 | CTRL_CH, S.knob_cc[i], out));
        show_view(V_KNOBS);
    }
}

void pads_level(uint8_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = pad[i].st == P_ON ? (pad[i].press ? pad[i].press : 1) : 0;
}

void knob_state(uint8_t pos[8], uint8_t value[8], uint8_t *rel) {
    *rel = 0;
    for (int i = 0; i < 8; i++) {
        pos[i] = kn[i].val;
        value[i] = S.knob_min[i] + kn[i].val * (S.knob_max[i] - S.knob_min[i]) / 127;
        if (S.knob_rel[i]) *rel |= 1u << i;
    }
}

/* ---- Joystick: centre = first reading (stick at rest at power-up); range grows with what is seen.
 * Per axis: JOY_PITCH = pitch bend, JOY_CC = one CC (_pos) over the whole range (centre 64),
 * JOY_DUAL = _neg CC for left/down and _pos CC for right/up, each 0-127 by distance from centre. ---- */
#define JOY_DEAD 80
#define JOY_HYST 12  /* per mille: at the edges the reading wobbles; v2.00 sent ~100 CC/s holding the stick there */
static struct { uint16_t c, lo, hi; int16_t lastr, pb, cc, neg, pos; uint8_t init; } joy[2];

static void joy_cc(int cc, int16_t *last, int v) {
    if (v != *last) *last = v, midi_push(pkt(0xB, 0xB0 | CTRL_CH, cc, v)), show_view(V_JOY);
}

void joy_state(int *x, int *y) { *x = joy[0].lastr, *y = joy[1].lastr; }

void joy_update(int ax, uint16_t v) {
    if (!joy[ax].init) {
        joy[ax].c = v, joy[ax].lo = v > 800 ? v - 800 : 0, joy[ax].hi = v < 3295 ? v + 800 : 4095;
        joy[ax].pb = 8192, joy[ax].cc = 64, joy[ax].neg = joy[ax].pos = 0, joy[ax].init = 1;
        return;
    }
    if (v < joy[ax].lo) joy[ax].lo = v;
    if (v > joy[ax].hi) joy[ax].hi = v;
    int d = v - joy[ax].c, r = 0;  /* r: -1000 .. 1000 */
    if (d > JOY_DEAD) r = (d - JOY_DEAD) * 1000 / (joy[ax].hi - joy[ax].c - JOY_DEAD);
    else if (d < -JOY_DEAD) r = (d + JOY_DEAD) * 1000 / (joy[ax].c - joy[ax].lo - JOY_DEAD);
    if (r && iabs(r - joy[ax].lastr) < JOY_HYST) return;  /* centre (r = 0) always gets through */
    joy[ax].lastr = r;
    int mode = ax ? S.joy_y_mode : S.joy_x_mode, neg = ax ? S.joy_y_neg : S.joy_x_neg, pos = ax ? S.joy_y_pos : S.joy_x_pos;
    if (mode == JOY_PITCH) {
        int pb = 8192 + r * 8191 / 1000;
        if (pb != joy[ax].pb) joy[ax].pb = pb, midi_push(pkt(0xE, 0xE0 | CTRL_CH, pb & 0x7F, pb >> 7)), show_view(V_JOY);
    } else if (mode == JOY_CC) {
        joy_cc(pos, &joy[ax].cc, 64 + r * 63 / 1000);
    } else {
        int vn = r < 0 ? -r * 127 / 1000 : 0, vp = r > 0 ? r * 127 / 1000 : 0;
        if (neg == pos) joy_cc(pos, &joy[ax].pos, vn > vp ? vn : vp);  /* same CC both ways (stock default) */
        else joy_cc(neg, &joy[ax].neg, vn), joy_cc(pos, &joy[ax].pos, vp);
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
