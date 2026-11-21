#include "menu.h"
#include "board.h"
#include "settings.h"

volatile uint8_t menu_on, menu_dirty, save_req;

enum { NUM, NAMES, NOTE, JOYX };
typedef struct {
    const char *name;
    uint8_t *v;
    uint8_t lo, hi, kind;
    int8_t off;                 /* NUM: shown value = v + off */
    const char *const *names;   /* NAMES */
} item_t;

static const char *const curves[] = {"SOFT", "NORMAL", "HARD", "FIXED"};
static const char *const speeds[] = {"SLOW", "NORMAL", "FAST"};
static const char *const yesno[] = {"NO", "YES"};
static const char *const offon[] = {"OFF", "ON"};
static const char *const arp_modes[] = {"UP", "DOWN", "UPDOWN EXC", "UPDOWN INC", "AS PLAYED", "RANDOM"};
static const char *const divisions[] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"};
static const char *const timeouts[] = {"5 S", "10 S", "30 S", "60 S", "NEVER"};
static const char *const clocks[] = {"INTERNAL", "EXTERNAL"};
static uint8_t reset;

static const item_t items[] = {
    {"KEYS CHANNEL", &S.keys_ch, 0, 15, NUM, 1, 0},
    {"PADS CHANNEL", &S.pads_ch, 0, 15, NUM, 1, 0},
    {"TRANSPOSE", &S.transpose, 0, 24, NUM, -12, 0},
    {"KEY VELOCITY", &S.key_curve, 0, 3, NAMES, 0, curves},
    {"PAD VELOCITY", &S.pad_curve, 0, 3, NAMES, 0, curves},
    {"PAD 1 NOTE", &S.pad_note, 0, 112, NOTE, 0, 0},
    {"KNOB 1 CC", &S.knob_cc[0], 0, 127, NUM, 0, 0},
    {"KNOB 2 CC", &S.knob_cc[1], 0, 127, NUM, 0, 0},
    {"KNOB 3 CC", &S.knob_cc[2], 0, 127, NUM, 0, 0},
    {"KNOB 4 CC", &S.knob_cc[3], 0, 127, NUM, 0, 0},
    {"KNOB 5 CC", &S.knob_cc[4], 0, 127, NUM, 0, 0},
    {"KNOB 6 CC", &S.knob_cc[5], 0, 127, NUM, 0, 0},
    {"KNOB 7 CC", &S.knob_cc[6], 0, 127, NUM, 0, 0},
    {"KNOB 8 CC", &S.knob_cc[7], 0, 127, NUM, 0, 0},
    {"KNOB SPEED", &S.knob_speed, 0, 2, NAMES, 0, speeds},
    {"JOYSTICK X", &S.joy_x_cc, 0, 127, JOYX, 0, 0},
    {"JOYSTICK Y CC", &S.joy_y_cc, 0, 127, NUM, 0, 0},
    {"CLOCK", &S.clock_ext, 0, 1, NAMES, 0, clocks},
    {"TEMPO BPM", &S.tempo, 40, 240, NUM, 0, 0},
    {"ARP MODE", &S.arp_mode, 0, 5, NAMES, 0, arp_modes},
    {"ARP/REPEAT RATE", &S.arp_div, 0, 7, NAMES, 0, divisions},
    {"ARP OCTAVES", &S.arp_oct, 0, 3, NUM, 1, 0},
    {"ARP LATCH", &S.arp_latch, 0, 1, NAMES, 0, offon},
    {"SCREEN OFF", &S.screen_off, 0, 4, NAMES, 0, timeouts},
    {"RESET ALL", &reset, 0, 1, NAMES, 0, yesno},
};
#define N_ITEMS ((int)(sizeof items / sizeof items[0]))
static int cur;

void menu_toggle(void) {
    if (!menu_on) {
        menu_on = 1, cur = 0, reset = 0;
    } else {
        menu_on = 0;
        if (reset) settings_defaults();
        save_req = 1;
    }
    menu_dirty = 1;
}

void menu_move(int d) { cur = (cur + d + N_ITEMS) % N_ITEMS; menu_dirty = 1; }

void menu_adjust(int d) {
    const item_t *it = &items[cur];
    int v = *it->v + d;
    *it->v = v < it->lo ? it->lo : v > it->hi ? it->hi : v;
    menu_dirty = 1;
}

static char *put_num(char *p, int v) {
    char tmp[4];
    int n = 0;
    if (v < 0) *p++ = '-', v = -v;
    do tmp[n++] = '0' + v % 10; while ((v /= 10) && n < 4);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

void menu_draw(void) {
    static const char *const notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const item_t *it = &items[cur];
    char buf[24], *p = buf;
    int v = *it->v;
    switch (it->kind) {
    case NAMES: p = put_str(p, it->names[v]); break;
    case NOTE: p = put_num(p, v); *p++ = ' '; p = put_str(p, notes[v % 12]); p = put_num(p, v / 12 - 2); break;
    case JOYX: p = v ? put_num(put_str(p, "CC "), v) : put_str(p, "PITCH BEND"); break;
    default: p = put_num(p, v + it->off);
    }
    *p = 0;

    char pos[8], *e = put_num(pos, cur + 1);
    *e++ = '/';
    e = put_num(e, N_ITEMS);
    *e = 0;

    fb_clear();
    text(0, 0, it->name, 1);
    text(128 - 6 * (int)(e - pos), 0, pos, 1);
    text(0, 14, buf, 2);
    oled_flush();
}
