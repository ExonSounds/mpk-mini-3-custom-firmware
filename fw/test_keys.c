/* Host test for main.c's key logic and velocity curve and analog.c's pads, knobs and joystick: make -C fw check */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define main fw_main
#include "main.c"
#undef main
#include "analog.c"
#include "ui.c"
#include "menu.c"
#include "settings.c"
#include "seq.c"
#include "cfg.c"

/* stubs for the board/USB functions main.c, analog.c, ui.c and menu.c link against */
int text(int x, int y, const char *s, int scale) { (void)y; (void)s; (void)scale; return x; }
void led_out(uint32_t bits) { (void)bits; }
void oled_power(int on) { (void)on; }
void adc_init(void) {}
void delay(volatile uint32_t n) { (void)n; }
void board_init(void) {}
void matrix_drive(int row) { (void)row; }
void usb_init(void) {}
int usb_configured(void) { return 0; }
int usb_midi_send(const uint32_t *p, int n) { (void)p; (void)n; return 0; }
void fb_clear(void) {}
void oled_flush(void) {}
void rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void digit(int x, int y, int d, int s) { (void)x; (void)y; (void)d; (void)s; }

static uint32_t pop(void) { assert(qt != qh); uint32_t p = q[qt]; qt = (qt + 1) & (QN - 1); return p; }
#define MS(x) ((uint32_t)(x) * (F_CPU / 1000))

static void buttons(uint16_t down) {  /* hold this button set long enough to pass the debounce */
    for (int i = 0; i < DEB; i++) { ui_button_row(8, down & 0xFF); ui_button_row(9, down >> 8); }
}
/* host side of the config protocol: split a SysEx into USB-MIDI packets, and reassemble the reply */
static void host_send(const uint8_t *m, unsigned len) {
    for (unsigned i = 0; i < len; i += 3) {
        unsigned n = len - i < 3 ? len - i : 3;
        uint32_t p = i + n < len ? 0x4 : 0x4 + n;
        for (unsigned j = 0; j < n; j++) p |= (uint32_t)m[i + j] << (8 * (j + 1));
        usb_midi_received(p);
    }
    cfg_poll();
}
static unsigned host_reply(uint8_t *out) {
    unsigned len = 0;
    while (qt != qh) {
        uint32_t p = pop();
        int n = (int[16]){[4] = 3, [5] = 1, [6] = 2, [7] = 3}[p & 0xF];
        assert(n);
        for (int j = 0; j < n; j++) out[len++] = p >> (8 * (j + 1));
        if (out[len - 1] == 0xF7) break;
    }
    return len;
}
static void hit(int p) { pad_update(p, 3000); for (int i = 0; i < PAD_PEAK; i++) pad_update(p, 3000); }
static void unhit(int p) { pad_update(p, 0); }

#define TRI(d) ((uint16_t)(fmod((d) + 3600.0, 360.0) < 180 ? fmod((d) + 3600.0, 360.0) / 180 * 4095 \
                                                               : (360 - fmod((d) + 3600.0, 360.0)) / 180 * 4095))

static void turn_knob1(double from, double to) {  /* knob 1 = analog item 0 */
    double step = from < to ? 2 : -2;
    for (double deg = from; step > 0 ? deg <= to : deg >= to; deg += step) knob_update(0, TRI(deg), TRI(deg + 90));
}

int main(void) {
    settings_defaults();
    assert(settings_valid(&S));
    assert(velocity(500) == 127 && velocity(2000) == 127 && velocity(5000) == 100 && velocity(200000) == 1);
    for (uint32_t us = 0, prev = 127; us < 200000; us += 250) { assert(velocity(us) <= prev); prev = velocity(us); }

    /* lowest C = key 0: first contact row 6 bit 3, second contact row 7 bit 3 */
    key_row(6, 1 << 3, MS(100));
    assert(qt == qh);                                    /* first contact alone: no note */
    key_row(7, 1 << 3, MS(110));                         /* 10 ms later: note on, vel 75 */
    assert(pop() == (0x09u | 0x90u << 8 | 48u << 16 | 75u << 24));
    key_row(7, 0, MS(200));
    assert(qt == qh);                                    /* second contact opening: nothing */
    key_row(6, 0, MS(210));                              /* first contact opens: note off */
    assert(pop() == (0x08u | 0x80u << 8 | 48u << 16 | 0x40u << 24));

    /* top C = key 24: rows 0/1 bit 3; DWT wraps around between contacts */
    key_row(0, 1 << 3, 0xFFFFFFFFu - MS(1));
    key_row(1, 1 << 3, MS(1));
    assert(pop() == (0x09u | 0x90u << 8 | 72u << 16 | 127u << 24));

    key_row(0, 0, MS(2));                                /* release top C */
    assert(pop() == (0x08u | 0x80u << 8 | 72u << 16 | 0x40u << 24));

    /* rows 0/1 bits 0-2 are not keys */
    key_row(0, 0x07, MS(300));
    key_row(1, 0x07, MS(301));
    assert(qt == qh);
    puts("key logic ok");

    /* pads: noise below threshold does nothing; a hit gives a note on ch 10 after the peak window, then off */
    for (int i = 0; i < 20; i++) pad_update(2, 100 + i * 10);
    assert(qt == qh);
    pad_update(2, 1200); pad_update(2, 3000); pad_update(2, 2600);
    assert(qt == qh);  /* still inside the peak window */
    pad_update(2, 2000);
    uint32_t on = pop();
    assert((on & 0xFFFFFF) == (0x09u | 0x99u << 8 | 38u << 16) && (on >> 24) == 1 + (3000 - 450) * 126 / (3800 - 450));
    pad_update(2, 600);
    assert(qt == qh);
    pad_update(2, 100);
    assert(pop() == (0x08u | 0x89u << 8 | 38u << 16 | 0x40u << 24));
    pad_update(5, 5000); pad_update(5, 5000); pad_update(5, 5000); pad_update(5, 5000);
    assert(pop() >> 24 == 127);  /* clamps */
    puts("pads ok");

    /* knobs: two triangle waves 90 deg apart; turning forward raises CC, backward lowers it, noise does nothing */
    knob_update(0, TRI(0), TRI(90));
    for (int i = 0; i < 2; i++) knob_update(0, TRI(0) + (i & 1) * 3, TRI(90));
    assert(qt == qh);
    int last = 0, n_up = 0;
    for (double deg = 0; deg <= 360; deg += 2) {
        knob_update(0, TRI(deg), TRI(deg + 90));
        while (qt != qh) { uint32_t p = pop(); int v = p >> 24; assert(((p >> 16) & 0xFF) == 1 && v > last); last = v; n_up++; }
    }
    assert(n_up > 50 && last >= 100);
    for (double deg = 360; deg >= 0; deg -= 2) {
        knob_update(0, TRI(deg), TRI(deg + 90));
        while (qt != qh) { int v = pop() >> 24; assert(v < last); last = v; }
    }
    assert(last <= 10);
    puts("knobs ok");

    /* joystick: X -> pitch bend around 8192, Y -> CC 1 both ways */
    joy_update(0, 2048);
    joy_update(0, 2048 + 50);
    assert(qt == qh);  /* dead zone */
    joy_update(0, 2048 + 800);
    uint32_t pb = pop();
    assert((pb & 0xFFFF) == (0x0Eu | 0xE0u << 8) && ((pb >> 16 & 0x7F) | (pb >> 24) << 7) == 16383);
    joy_update(0, 2048);
    pb = pop();
    assert(((pb >> 16 & 0x7F) | (pb >> 24) << 7) == 8192);
    joy_update(1, 2000);
    joy_update(1, 2000 - 800);
    assert(pop() == (0x0Bu | 0xB0u << 8 | 1u << 16 | 127u << 24));
    for (int i = 0; i < 20; i++) joy_update(1, 2000 - 800 + (i & 1) * 6);
    assert(qt == qh);  /* wobble at the edge: nothing */
    joy_update(1, 2000 + 800);
    assert(qt == qh);  /* same CC value (127) the other way: nothing new */
    joy_update(1, 2000);
    assert(pop() == (0x0Bu | 0xB0u << 8 | 1u << 16 | 0u << 24));
    puts("joystick ok");

    /* buttons: a short bounce does nothing; OCT+ shifts the next note; release keeps the original note */
    ui_button_row(8, 1 << BTN_OCT_UP); ui_button_row(8, 0);
    for (int i = 0; i < 2 * DEB; i++) ui_button_row(8, 0);
    assert(ui_octave == 0);
    buttons(1 << BTN_OCT_UP); buttons(0);
    assert(ui_octave == 1);
    key_row(6, 1 << 3, MS(400)); key_row(7, 1 << 3, MS(410));
    assert((pop() >> 16 & 0xFF) == 60);
    buttons(1 << BTN_OCT_DOWN); buttons(0);
    assert(ui_octave == 0);
    key_row(6, 0, MS(420));
    assert(pop() == (0x08u | 0x80u << 8 | 60u << 16 | 0x40u << 24));  /* off for the note that was sent */
    for (int i = 0; i < 6; i++) { buttons(1 << BTN_OCT_DOWN); buttons(0); }
    assert(ui_octave == -4);                                           /* clamps */
    buttons(1 << BTN_OCT_DOWN); buttons(1 << BTN_OCT_DOWN | 1 << BTN_OCT_UP); buttons(0);
    assert(ui_octave == 0);                                            /* both together: reset */

    /* pads: bank B, full level, CC and program-change modes; release matches what was sent */
    buttons(1 << BTN_PAD_BANK); buttons(0);
    hit(0);
    on = pop();
    assert((on >> 16 & 0xFF) == 44 && (on >> 8 & 0xFF) == 0x99);
    buttons(1 << BTN_PAD_BANK); buttons(0);                             /* back to A while pad held */
    unhit(0);
    assert(pop() == (0x08u | 0x89u << 8 | 44u << 16 | 0x40u << 24));
    buttons(1 << (BTN_FULL_LEVEL)); buttons(0);
    hit(1);
    assert(pop() == (0x09u | 0x99u << 8 | 37u << 16 | 127u << 24));
    unhit(1); pop();
    buttons(1 << BTN_FULL_LEVEL); buttons(0);
    buttons(1 << BTN_CC); buttons(0);
    hit(1);
    assert((pop() & 0xFFFFFF) == (0x0Bu | 0xB9u << 8 | 17u << 16));
    unhit(1);
    assert(pop() == (0x0Bu | 0xB9u << 8 | 17u << 16 | 0u << 24));
    buttons(1 << BTN_PROG_CHANGE); buttons(0);
    hit(1);
    assert(pop() == (0x0Cu | 0xC9u << 8 | 1u << 16));
    unhit(1);
    assert(qt == qh);                                                   /* program change: nothing on release */
    buttons(1 << BTN_PROG_CHANGE); buttons(0);
    assert(ui_pad_mode == PADS_NOTE);
    puts("buttons ok");

    /* curves */
    for (int v = 1; v <= 127; v++) {
        assert(curve(v, CURVE_SOFT) >= v && curve(v, CURVE_HARD) <= v && curve(v, CURVE_FIXED) == 127);
        if (v > 1) assert(curve(v, CURVE_SOFT) >= curve(v - 1, CURVE_SOFT) && curve(v, CURVE_HARD) >= curve(v - 1, CURVE_HARD));
    }
    assert(curve(127, CURVE_SOFT) == 127 && curve(127, CURVE_HARD) == 127 && curve(1, CURVE_HARD) == 1);

    /* menu: PROG SELECT opens, OCT+ goes to PADS CHANNEL, knob 1 changes it without sending CC; close = save */
    kn[0].init = 0;
    knob_update(0, TRI(0), TRI(90));
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    assert(menu_on && cur == 0);
    buttons(1 << BTN_OCT_UP); buttons(0);
    assert(cur == 1 && ui_octave == 0);           /* OCT+ navigates instead of shifting */
    turn_knob1(0, 30);
    assert(qt == qh && S.pads_ch > 9);            /* no CC sent while editing */
    turn_knob1(30, -360);
    assert(S.pads_ch == 0);                       /* clamps at channel 1 */
    S.pads_ch = 4;                                /* channel 5 */
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    assert(!menu_on && save_req);
    save_req = 0;
    hit(3);
    assert((pop() & 0xFFFF) == (0x09u | 0x94u << 8));  /* pad note on channel 5 */
    unhit(3); pop();

    /* transpose and key channel */
    S.transpose = 14, S.keys_ch = 2;
    key_row(6, 1 << 3, MS(500)); key_row(7, 1 << 3, MS(505));
    assert((pop() & 0xFFFFFF) == (0x09u | 0x92u << 8 | 50u << 16));
    key_row(6, 0, MS(510));
    assert(pop() == (0x08u | 0x82u << 8 | 50u << 16 | 0x40u << 24));

    /* RESET ALL (last item, OCT- from the first wraps there) restores defaults on close */
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    buttons(1 << BTN_OCT_DOWN); buttons(0);
    assert(cur == N_ITEMS - 1);
    menu_adjust(1);
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    assert(S.keys_ch == 0 && S.transpose == 12 && S.pads_ch == 9 && save_req);
    puts("menu ok");

    /* arpeggiator: 130 BPM, 1/16 = 24 pulses, gate 12; ticks per pulse = 1.2M / (130 * 96) ~ 96.2 */
    #define PULSES(n) for (int t = 0; t < (int)((n) * 1200000.0 / (S.tempo * 96)) + 1; t++) seq_tick()
    while (qt != qh) pop();
    buttons(1 << BTN_ARP); buttons(0);
    assert(seq_arp_on);
    key_row(6, 1 << 3 | 1 << 7, MS(600));                /* keys 0 (C3) and 4 (E3, row 4/5 bit 7... */
    key_row(4, 1 << 7, MS(600));                        /*  ...is first contact row 4) */
    key_row(7, 1 << 3, MS(605));                        /* C3 note-on -> arp: sounds at once */
    assert(pop() == (0x09u | 0x90u << 8 | 48u << 16 | (uint32_t)curve(velocity(5000), S.key_curve) << 24));
    key_row(5, 1 << 7, MS(606));                        /* E3 joins */
    assert(qt == qh);
    PULSES(12);
    assert(pop() == (0x08u | 0x80u << 8 | 48u << 16 | 0x40u << 24));  /* gate end */
    PULSES(12);
    assert((pop() & 0xFFFFFF) == (0x09u | 0x90u << 8 | 52u << 16));     /* next step: E3 */
    PULSES(24);
    assert((pop() & 0xFFFFFF) == (0x08u | 0x80u << 8 | 52u << 16));
    assert((pop() & 0xFFFFFF) == (0x09u | 0x90u << 8 | 48u << 16));     /* UP wraps to C3 */
    /* DOWN over 2 octaves: 64 60 52 48 */
    S.arp_mode = ARP_DOWN, S.arp_oct = 1;
    int seen[4];
    for (int i = 0; i < 4; i++) {
        PULSES(24);
        uint32_t a = pop(), b2 = pop();
        assert((a >> 8 & 0xF0) == 0x80 && (b2 >> 8 & 0xF0) == 0x90);
        seen[i] = b2 >> 16 & 0xFF;
    }
    /* step counter carries on from UP: find 64 and check the order after it */
    int st = 0; while (seen[st] != 64) st++;
    for (int i = 1; i < 4 && st + i < 4; i++) assert((seen[st + i] == (int[]){64, 60, 52, 48}[i]));
    /* release both keys (latch off): note off, then silence */
    key_row(6, 0, MS(700)); key_row(4, 0, MS(700));
    while (qt != qh) assert((pop() >> 8 & 0xF0) == 0x80);
    PULSES(96);
    assert(qt == qh);
    /* latch: chord keeps playing after release */
    S.arp_latch = 1, S.arp_mode = ARP_UP, S.arp_oct = 0;
    key_row(6, 1 << 3, MS(800)); key_row(7, 1 << 3, MS(801));
    assert((pop() >> 16 & 0xFF) == 48);
    key_row(6, 0, MS(810));
    PULSES(48);
    int ons = 0;
    while (qt != qh) ons += ((pop() >> 8 & 0xF0) == 0x90);
    assert(ons >= 1);
    buttons(1 << BTN_ARP); buttons(0);                  /* ARP off: last note off, silence */
    assert(!seq_arp_on);
    while (qt != qh) pop();
    PULSES(96);
    assert(qt == qh);
    S.arp_latch = 0;
    puts("arp ok");

    /* note repeat: pad retriggers every step while held */
    buttons(1 << BTN_NOTE_REPEAT); buttons(0);
    assert(seq_repeat_on);
    hit(6);
    assert((pop() & 0xFFFFFF) == (0x09u | 0x99u << 8 | 42u << 16));
    PULSES(12);
    assert(pop() == (0x08u | 0x89u << 8 | 42u << 16 | 0x40u << 24));
    PULSES(12);
    assert((pop() & 0xFFFFFF) == (0x09u | 0x99u << 8 | 42u << 16));
    unhit(6);
    assert(pop() == (0x08u | 0x89u << 8 | 42u << 16 | 0x40u << 24));
    PULSES(96);
    assert(qt == qh);
    buttons(1 << BTN_NOTE_REPEAT); buttons(0);
    puts("note repeat ok");

    /* tap tempo: taps 0.5 s apart -> 120 BPM; a long pause starts over */
    for (int t = 0; t < 3 * 20000; t++) seq_tick();
    for (int tap = 0; tap < 4; tap++) {
        buttons(1 << BTN_TAP_TEMPO); buttons(0);
        for (int t = 0; t < 10000 - 2 * 2 * DEB; t++) seq_tick();  /* buttons() takes no seq ticks */
    }
    assert(S.tempo >= 119 && S.tempo <= 121);
    puts("tap tempo ok");

    /* FULL LEVEL also applies to the keys */
    while (qt != qh) pop();
    buttons(1 << BTN_FULL_LEVEL); buttons(0);
    key_row(6, 1 << 3, MS(900)); key_row(7, 1 << 3, MS(960));  /* slow press would be soft */
    assert(pop() >> 24 == 127);
    key_row(6, 0, MS(970)); pop();
    buttons(1 << BTN_FULL_LEVEL); buttons(0);
    key_row(6, 1 << 3, MS(980)); key_row(7, 1 << 3, MS(1040));
    assert(pop() >> 24 < 127);
    key_row(6, 0, MS(1050)); pop();
    puts("full level keys ok");

    /* external clock: steps follow F8 clocks (1/16 = 6 clocks), Start restarts, no clock = no steps, Stop silences */
    #define RT(b) usb_midi_received(0x0Fu | (uint32_t)(b) << 8)
    #define CLOCKS(n) for (int c = 0; c < (n); c++) { for (int t = 0; t < 200; t++) seq_tick(); RT(0xF8); }
    S.clock_ext = 1;
    int tempo_before = S.tempo;
    buttons(1 << BTN_TAP_TEMPO); buttons(0); for (int t = 0; t < 8000; t++) seq_tick();
    buttons(1 << BTN_TAP_TEMPO); buttons(0);
    assert(S.tempo == tempo_before);                       /* taps ignored on external clock */
    buttons(1 << BTN_ARP); buttons(0);
    key_row(6, 1 << 3, MS(1100)); key_row(7, 1 << 3, MS(1105));
    assert((pop() >> 16 & 0xFF) == 48);                    /* first note at once */
    for (int t = 0; t < 20000; t++) seq_tick();
    assert(qt == qh);                                      /* no clock: nothing moves */
    RT(0xFA);                                              /* Start: off + first step */
    assert((pop() >> 8 & 0xF0) == 0x80 && (pop() >> 8 & 0xF0) == 0x90);
    CLOCKS(2);
    assert(qt == qh);
    CLOCKS(2);                                             /* gate end inside the 3rd clock's pulses */
    assert((pop() >> 8 & 0xF0) == 0x80 && qt == qh);
    CLOCKS(2);                                             /* 6 clocks = next step */
    assert((pop() >> 8 & 0xF0) == 0x90);
    CLOCKS(6);
    assert((pop() >> 8 & 0xF0) == 0x80 && (pop() >> 8 & 0xF0) == 0x90 && qt == qh);
    RT(0xFC);                                              /* Stop */
    assert((pop() >> 8 & 0xF0) == 0x80);
    CLOCKS(24);
    assert(qt == qh);                                      /* stopped: clocks ignored */
    RT(0xFB);                                              /* Continue */
    CLOCKS(6);
    assert(qt != qh);
    key_row(6, 0, MS(1200));
    buttons(1 << BTN_ARP); buttons(0);
    while (qt != qh) pop();
    S.clock_ext = 0;
    puts("external clock ok");

    /* config protocol */
    settings_defaults();
    uint8_t r[80];
    const uint8_t hello[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x01, 0xF7};
    host_send(hello, sizeof hello);
    assert(host_reply(r) == 10 && r[4] == 0x41 && r[7] == SETTINGS_LAYOUT && r[8] == sizeof(settings_t) && r[9] == 0xF7);
    const uint8_t get[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x02, 0xF7};
    host_send(get, sizeof get);
    assert(host_reply(r) == 6 + 2 * sizeof(settings_t) && r[4] == 0x42);
    for (unsigned i = 0; i < sizeof S; i++) assert((r[5 + 2 * i] << 4 | r[6 + 2 * i]) == ((uint8_t *)&S)[i]);
    /* SET: change keys channel to 6 and tempo to 200 (> 127, needs the nibbles) */
    uint8_t set[6 + 2 * sizeof(settings_t)] = {0xF0, 0x7D, 0x4D, 0x4B, 0x03};
    settings_t t = S;
    t.keys_ch = 5, t.tempo = 200;
    for (unsigned i = 0; i < sizeof t; i++) set[5 + 2 * i] = ((uint8_t *)&t)[i] >> 4, set[6 + 2 * i] = ((uint8_t *)&t)[i] & 15;
    set[sizeof set - 1] = 0xF7;
    host_send(set, sizeof set);
    assert(host_reply(r) == 7 && r[4] == 0x43 && r[5] == 0 && S.keys_ch == 5 && S.tempo == 200);
    t.keys_ch = 20;  /* invalid */
    set[5] = t.keys_ch >> 4, set[6] = t.keys_ch & 15;
    host_send(set, sizeof set);
    assert(host_reply(r) == 7 && r[5] == 2 && S.keys_ch == 5);
    const uint8_t short_set[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x03, 0, 1, 0xF7};
    host_send(short_set, sizeof short_set);
    assert(host_reply(r) == 7 && r[5] == 1);
    const uint8_t defaults[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x05, 0xF7};
    host_send(defaults, sizeof defaults);
    assert(host_reply(r) == 7 && r[4] == 0x45 && r[5] == 0 && S.keys_ch == 0 && S.tempo == 130);
    const uint8_t unknown[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x33, 0xF7};
    host_send(unknown, sizeof unknown);
    assert(host_reply(r) == 7 && r[4] == 0x73 && r[5] == 0x7F);
    const uint8_t other[] = {0xF0, 0x47, 0x7F, 0x49, 0x66, 0x00, 0x01, 0x01, 0xF7};  /* Akai's own query: ignored */
    host_send(other, sizeof other);
    assert(qt == qh);
    puts("config protocol ok");

    /* ---- layout 4: per-pad tables, aftertouch, knob range/relative, joystick modes, swing/gate ---- */
    settings_defaults();
    while (qt != qh) pop();
    S.pad_note[2] = 60, S.pad_cc[2] = 99, S.pad_pc[2] = 42, S.pad_note[8 + 2] = 70;
    hit(2); assert((pop() >> 16 & 0xFF) == 60); unhit(2); pop();
    buttons(1 << BTN_PAD_BANK); buttons(0);
    hit(2); assert((pop() >> 16 & 0xFF) == 70); unhit(2); pop();
    buttons(1 << BTN_PAD_BANK); buttons(0);
    buttons(1 << BTN_CC); buttons(0);
    hit(2); assert((pop() >> 16 & 0xFF) == 99); unhit(2); pop();
    buttons(1 << BTN_CC); buttons(0); buttons(1 << BTN_PROG_CHANGE); buttons(0);
    hit(2); assert((pop() >> 16 & 0xFF) == 42); unhit(2);
    buttons(1 << BTN_PROG_CHANGE); buttons(0);
    while (qt != qh) pop();
    /* channel aftertouch: firmest held pad; back to 0 on release */
    S.pad_at = 1;
    hit(2); pop();
    pad_update(2, 2000);
    uint32_t at = pop();
    assert((at & 0xFFFF) == (0x0Du | 0xD9u << 8) && (at >> 16 & 0x7F) == (2000 - 250) * 127 / (3800 - 250));
    pad_update(2, 2005);
    assert(qt == qh);                                       /* noise below 2 steps */
    unhit(2);
    assert(pop() == (0x0Du | 0xD9u << 8 | 0u << 16));       /* pressure back to 0 ... */
    assert((pop() >> 8 & 0xF0) == 0x80);                    /* ... then note off */
    S.pad_at = 2;                                           /* poly: per note */
    hit(2); pop();
    pad_update(2, 3800);
    assert(pop() == (0x0Au | 0xA9u << 8 | 60u << 16 | 127u << 24));
    unhit(2); pop(); pop();
    S.pad_at = 0;
    puts("pads layout 4 ok");

    /* knobs: range 20..40, reversed 100..0, relative mode */
    kn[1].init = 0; knob_update(1, TRI(0), TRI(90));
    S.knob_min[1] = 20, S.knob_max[1] = 40;
    for (double deg = 0; deg <= 360; deg += 2) knob_update(1, TRI(deg), TRI(deg + 90));
    int lo = 127, hi = 0;
    while (qt != qh) { int v = pop() >> 24; lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
    assert(lo >= 20 && hi <= 40 && hi > lo);
    kn[2].init = 0; knob_update(2, TRI(0), TRI(90));
    S.knob_min[2] = 100, S.knob_max[2] = 0;
    for (double deg = 0; deg <= 90; deg += 2) knob_update(2, TRI(deg), TRI(deg + 90));
    { int first = -1, last = -1; while (qt != qh) { int v = pop() >> 24; if (first < 0) first = v; last = v; }
      assert(first <= 100 && last < first); }               /* turning up moves the value down */
    kn[3].init = 0; knob_update(3, TRI(0), TRI(90));
    S.knob_rel[3] = 1;
    for (double deg = 0; deg <= 30; deg += 2) knob_update(3, TRI(deg), TRI(deg + 90));
    while (qt != qh) assert((pop() >> 24) > 64);            /* clockwise = above 64 */
    for (double deg = 30; deg >= 0; deg -= 2) knob_update(3, TRI(deg), TRI(deg + 90));
    while (qt != qh) assert((pop() >> 24) < 64);
    S.knob_rel[3] = 0;
    puts("knobs layout 4 ok");

    /* joystick: X as one CC (centre 64), Y with separate down/up CCs on the knob channel */
    S.ctrl_ch = 5;
    S.joy_x_mode = JOY_CC, S.joy_x_pos = 20;
    joy_update(0, 2048 + 800);
    assert(pop() == (0x0Bu | 0xB5u << 8 | 20u << 16 | 127u << 24));
    joy_update(0, 2048 - 800);
    assert(pop() == (0x0Bu | 0xB5u << 8 | 20u << 16 | 1u << 24));
    joy_update(0, 2048);
    assert(pop() >> 24 == 64);
    S.joy_y_mode = JOY_DUAL, S.joy_y_neg = 30, S.joy_y_pos = 31;
    joy_update(1, 2000 + 800);
    assert(pop() == (0x0Bu | 0xB5u << 8 | 31u << 16 | 127u << 24) && qt == qh);
    joy_update(1, 2000 - 800);
    uint32_t a1 = pop(), a2 = pop();                        /* down CC up, up CC back to 0 */
    assert(a1 == (0x0Bu | 0xB5u << 8 | 30u << 16 | 127u << 24) && a2 == (0x0Bu | 0xB5u << 8 | 31u << 16));
    joy_update(1, 2000);
    assert(pop() == (0x0Bu | 0xB5u << 8 | 30u << 16) && qt == qh);
    settings_defaults();
    puts("joystick layout 4 ok");

    /* swing 75%: 1/16 steps alternate 36 and 12 pulses; gate 25% ends a step after a quarter of it */
    S.swing = 75, S.gate = 25;
    buttons(1 << BTN_ARP); buttons(0);
    key_row(6, 1 << 3, MS(1300)); key_row(7, 1 << 3, MS(1305));
    pop();                                                  /* first note at once (step 0, even) */
    PULSES(8); assert(qt == qh);
    PULSES(1); assert((pop() >> 8 & 0xF0) == 0x80);          /* gate 36 * 25% = 9 pulses */
    PULSES(26); assert(qt == qh);
    PULSES(1); assert((pop() >> 8 & 0xF0) == 0x90);          /* odd step starts at 36 */
    PULSES(3); assert((pop() >> 8 & 0xF0) == 0x80);          /* gate 12 * 25% = 3 */
    PULSES(8); assert(qt == qh);
    PULSES(1); assert((pop() >> 8 & 0xF0) == 0x90 && qt == qh);  /* next even step at 48 */
    key_row(6, 0, MS(1400));
    buttons(1 << BTN_ARP); buttons(0);
    while (qt != qh) pop();
    settings_defaults();
    puts("swing/gate ok");

    /* menu shortcut: PAD NOTES FROM rewrites all 16 pad notes; leaving it alone keeps custom ones */
    S.pad_note[5] = 1;
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    assert(S.pad_note[5] == 1);
    save_req = 0;
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    while (items[cur].v != &pad_base) { buttons(1 << BTN_OCT_UP); buttons(0); }
    menu_adjust(12);
    assert(S.pad_note[0] == 48 && S.pad_note[5] == 53 && S.pad_note[15] == 63);
    buttons(1 << BTN_PROG_SELECT); buttons(0);
    save_req = 0;
    puts("menu layout 4 ok");
    return 0;
}
