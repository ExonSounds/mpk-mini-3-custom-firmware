/* Host test for main.c's key logic and velocity curve and analog.c's pads, knobs and joystick: make -C fw check */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define main fw_main
#include "main.c"
#undef main
#include "analog.c"
#include "ui.c"
#include "menu.c"
#include "settings.c"
#include "seq.c"
#include "cfg.c"
#include "pong.c"
#include "views.c"
#include "gfx.c"
#include "prog.c"

static int resets;
void system_reset(void) { resets++; }
static unsigned test_echo(const uint8_t *in, unsigned n, uint8_t *out) {  /* answers: n, last byte, 0xFF masked */
    out[0] = n, out[1] = in[n - 1], out[2] = 0xFF;
    return 3;
}

/* stubs for the board/USB functions main.c, analog.c, ui.c and menu.c link against */
static uint32_t led_bits;
void led_out(uint32_t bits) { led_bits = bits; }
void oled_power(int on) { (void)on; }
void adc_init(void) {}
void delay(volatile uint32_t n) { (void)n; }
void board_init(void) {}
void matrix_drive(int row) { (void)row; }
void usb_init(void) {}
int usb_configured(void) { return 0; }
int usb_midi_send(const uint32_t *p, int n) { (void)p; (void)n; return 0; }
void oled_flush(void) {}

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

    /* ping-pong idle screen: everything stays on screen, rallies happen, both sides score, score wraps after 9 */
    assert(S.idle_screen == 0);                          /* default = ping-pong */
    pong_reset();
    int bounces = 0, points[2] = {0, 0}, wraps = 0;
    for (int f = 0; f < 20000; f++) {
        int pvx = vx, s0 = score[0], s1 = score[1];
        pong_step();
        assert(py[0] >= 0 && py[0] <= (H - PH) * FP && py[1] >= 0 && py[1] <= (H - PH) * FP);
        assert(by >= 0 && by <= (H - 2) * FP && bx >= -3 * FP && bx <= (W + 3) * FP);
        if ((pvx < 0) != (vx < 0) && score[0] == s0 && score[1] == s1) bounces++;
        if (score[0] != s0 || score[1] != s1) {
            if (score[0] == 0 && score[1] == 0) wraps++;
            else points[score[1] != s1]++;
        }
    }
    assert(bounces > 50 && points[0] > 0 && points[1] > 0 && wraps > 0);
    printf("pong ok (%d returns, %d:%d points, %d games)\n", bounces, points[0], points[1], wraps);

    /* live views: check the pixels */
    #define LIT(x, y) (fb[((y) >> 3) * 128 + (x)] >> ((y) & 7) & 1)
    uint8_t kl[25] = {0}, pl[8] = {0};
    draw_keys(kl);
    assert(!LIT(KX + 4, 25) && LIT(KX, 25) && LIT(KX + KW, 25));         /* C3 open, key borders drawn */
    assert(LIT(KX + KW - 2, 0) && LIT(KX + KW - 2, 10) && !LIT(KX + KW, 10));  /* C#3: outline, open middle */
    kl[0] = kl[1] = kl[24] = 127;
    draw_keys(kl);                                                      /* C3, C#3 and top C held hard */
    assert(LIT(KX + 4, 25) && LIT(KX + 14 * KW + 4, 25));              /* white keys filled below the black keys */
    assert(LIT(KX + KW, 10) && LIT(KX + KW - 3, 10) == 0);              /* C#3 filled, with a dark gap to C3 */
    assert(!LIT(KX + 2 * KW + 4, 25));                                  /* D3 not held */
    pl[4] = 127;
    draw_pads(pl, 0);                                                   /* pad 5 = top left, full pressure */
    assert(LIT(15, 7) && LIT(1, 0) && LIT(30, 14));                     /* filled */
    assert(LIT(1, 17) && !LIT(2, 18) && !LIT(5, 30));                   /* pad 1 bottom left: outline only */
    int lab = 0; for (int x = 10; x < 22; x++) for (int y = 21; y < 28; y++) lab |= LIT(x, y);
    assert(lab);                                                        /* its "A1" label */
    uint8_t pos[8] = {0, 127, 64}, value[8] = {0, 127, 64};
    draw_knobs(pos, value, 1u << 7);
    assert(LIT(8 - 4, 9 + 4) && LIT(24 + 4, 9 + 4) && LIT(40, 9 - 5));  /* pointers: min, max, centre */
    assert(LIT(8, 9 - 7) && LIT(8 + 7, 9));                             /* circle */
    assert(LIT(120, 9 - 5));                                            /* relative knob 8: pointer up */
    int num8 = 0; for (int x = 114; x < 127; x++) for (int y = 21; y < 26; y++) num8 |= LIT(x, y);
    assert(!num8);                                                      /* and no value under it */
    draw_joystick(0, 0);
    assert(LIT(64, 16) && LIT(63, 15) && LIT(48, 0) && LIT(79, 31));    /* centred dot, box corners */
    draw_joystick(1000, 1000);
    assert(LIT(78, 2) && !LIT(64, 16));                                 /* full right + up = top right */
    int xr = 0; for (int x = 0; x < 48; x++) for (int y = 21; y < 28; y++) xr |= LIT(x, y);
    assert(xr);                                                         /* X readout ("100") drawn */
    /* dither: lit share of the pad's inside follows the pressure; the lightest touch still shows */
    int lit_at[3]; const uint8_t lv[3] = {1, 64, 127};
    for (int t = 0; t < 3; t++) {
        pl[4] = lv[t];
        draw_pads(pl, 0);
        lit_at[t] = 0;
        for (int x = 2; x < 30; x++) for (int y = 1; y < 14; y++) lit_at[t] += LIT(x, y);
    }
    assert(lit_at[0] > 0 && lit_at[0] < 364 / 8 && lit_at[1] > 364 * 3 / 8 && lit_at[1] < 364 * 5 / 8 && lit_at[2] == 364);
    /* PWM: the whole inside on or off per frame; lit in 1, 2 and 4 of the 4 frames for the same pressures */
    views_pwm = 1;
    for (int t = 0; t < 3; t++) {
        int frames = 0;
        pl[4] = lv[t];
        for (int f = 0; f < 4; f++, views_phase++) {
            draw_pads(pl, 0);
            int n = 0;
            for (int x = 2; x < 30; x++) for (int y = 1; y < 14; y++) n += LIT(x, y);
            assert(n == 0 || n == 364);                                 /* no dot pattern */
            frames += n == 364;
        }
        static const int want[3] = {1, 2, 4};
        assert(frames == want[t]);
    }
    views_pwm = 0;
    puts("views ok");

    /* status messages, tempo screen, BPM from external clock, home screen */
    settings_defaults();
    while (qt != qh) pop();
    buttons(1 << BTN_ARP); buttons(0);
    assert(view == V_MSG && !strcmp(msg_title, "ARPEGGIATOR") && !strcmp(msg_value, "ON"));
    buttons(1 << BTN_ARP); buttons(0);
    assert(!strcmp(msg_value, "OFF"));
    buttons(1 << BTN_OCT_UP); buttons(0);
    assert(!strcmp(msg_title, "OCTAVE") && !strcmp(msg_value, "+1"));
    buttons(1 << BTN_OCT_UP | 1 << BTN_OCT_DOWN); buttons(0);
    assert(!strcmp(msg_value, "0") && ui_octave == 0);
    buttons(1 << BTN_PROG_CHANGE); buttons(0);
    assert(!strcmp(msg_value, "PROGRAM"));
    buttons(1 << BTN_PROG_CHANGE); buttons(0);
    assert(!strcmp(msg_value, "NOTES"));
    buttons(1 << BTN_TAP_TEMPO); buttons(0);
    assert(view == V_TEMPO && seq_bpm() == S.tempo);
    S.clock_ext = 1;
    for (int c = 0; c < 48; c++) { for (int t = 0; t < 200; t++) seq_tick(); RT(0xF8); }
    assert(seq_bpm() == 250);                          /* 24 clocks per quarter, one every 10 ms */
    for (int t = 0; t < 21000; t++) seq_tick();
    assert(seq_bpm() == 0);                            /* clock stopped: shown as --- */
    while (qt != qh) pop();
    S.clock_ext = 0;
    home_t h = {.octave = -2, .bank = 1, .pad_mode = 1, .arp = 1, .full = 1, .usb = 1, .bpm = 128};
    draw_home(&h);
    int l1 = 0, l3 = 0;
    for (int x = 0; x < 120; x++) {
        for (int y = 1; y < 8; y++) l1 |= LIT(x, y);
        for (int y = 24; y < 31; y++) l3 |= LIT(x, y);
    }
    assert(l1 && l3 && LIT(124, 28));                  /* status lines and the USB square */
    draw_tempo(128, 0, 1);
    assert(LIT(115, 17) && LIT(8, 20));                /* beat dot on, big digits (stem of the "1") */
    draw_tempo(128, 0, 0);
    assert(!LIT(115, 17) && LIT(110, 12));             /* beat dot off: outline only */
    puts("status screens ok");

    /* idle image over SysEx: write 8 chunks, read one back, reject a bad offset, clear; inversion */
    uint8_t m[6 + 2 + 128];
    m[0] = 0xF0, m[1] = 0x7D, m[2] = 0x4D, m[3] = 0x4B, m[4] = 0x06, m[sizeof m - 1] = 0xF7;
    for (int off = 0; off < 512; off += 64) {
        m[5] = off >> 7, m[6] = off & 127;
        for (int i = 0; i < 64; i++) m[7 + 2 * i] = (off / 64 + i) >> 4 & 15, m[8 + 2 * i] = (off / 64 + i) & 15;
        host_send(m, sizeof m);
        assert(host_reply(r) == 7 && r[5] == 0);
    }
    assert(idle_image_set && idle_image[0] == 0 && idle_image[64] == 1 && idle_image[511] == 7 + 63);
    const uint8_t rd[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x07, 0, 64, 0xF7};
    host_send(rd, sizeof rd);
    uint8_t big[160];
    assert(host_reply(big) == 6 + 129 && big[4] == 0x47 && big[5] == 1);
    for (int i = 0; i < 64; i++) assert((big[6 + 2 * i] << 4 | big[7 + 2 * i]) == 1 + i);
    m[5] = 4, m[6] = 1;                                  /* offset 513: refused */
    host_send(m, sizeof m);
    assert(host_reply(r) == 7 && r[5] == 1);
    draw_idle_image(idle_image, 0);
    assert(fb[64] == 1 && fb[0] == 0);
    draw_idle_image(idle_image, 1);
    assert(fb[64] == 0xFE && fb[0] == 0xFF);             /* inverted against burn-in */
    const uint8_t clr[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x08, 0xF7};
    host_send(clr, sizeof clr);
    assert(host_reply(r) == 7 && r[5] == 0 && !idle_image_set);
    draw_idle_image(0, 0);                               /* built-in logo */
    int logo = 0; for (int i = 0; i < 512; i++) logo += fb[i] != 0;
    assert(logo > 20);
    S.idle_screen = IDLE_IMAGE;
    assert(settings_valid(&S));
    S.idle_screen = 3;
    assert(!settings_valid(&S));
    settings_defaults();
    puts("idle image ok");

    for (int i = 0; i < 4 * 32 * 2; i++) ui_led_tick();  /* defaults: BANK A LED is lit */
    uint32_t lit = 0; for (int i = 0; i < 4 * 32; i++) ui_led_tick(), lit |= led_bits;
    ui_leds_off = 1;                                     /* idle screen OFF: every LED dark for a whole period */
    for (int i = 0; i < 8; i++) ui_led_tick();
    uint32_t dark = 0; for (int i = 0; i < 4 * 32; i++) ui_led_tick(), dark |= led_bits;
    ui_leds_off = 0;
    uint32_t back = 0; for (int i = 0; i < 4 * 32; i++) ui_led_tick(), back |= led_bits;
    assert(lit && !dark && back == lit);
    puts("leds off ok");

    /* RAM program upload: one 64-byte block with header (magic, entry = PROG_BASE + 9, Thumb) */
    uint8_t prog[64] = {0x4D, 0x4B, 0x52, 0x50, 0x09, 0x20, 0x00, 0x20, 0x70, 0x47};  /* ..., bx lr */
    uint8_t pm[6 + 2 + 128], pr[16];
    memcpy(pm, (uint8_t[]){0xF0, 0x7D, 0x4D, 0x4B, 0x09, 0, 0}, 7);
    for (int i = 0; i < 64; i++) pm[7 + 2 * i] = prog[i] >> 4, pm[8 + 2 * i] = prog[i] & 15;
    pm[sizeof pm - 1] = 0xF7;
    host_send(pm, sizeof pm);
    assert(host_reply(pr) == 7 && pr[4] == 0x49 && pr[5] == 0);
    unsigned psum = 0; for (int i = 0; i < 64; i++) psum += prog[i];
    uint8_t run[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x0A, 0, 0, 64, psum >> 14 & 127, psum >> 7 & 127, (psum + 1) & 127, 0xF7};
    host_send(run, sizeof run);
    assert(host_reply(pr) == 7 && pr[5] == 2 && !prog_run_req);       /* wrong checksum: refused */
    run[10] = psum & 127;
    host_send(run, sizeof run);
    assert(host_reply(pr) == 7 && pr[5] == 0 && prog_run_req);        /* ok: main loop starts it */
    host_send(pm, sizeof pm);
    assert(host_reply(pr) == 7 && pr[5] == 3);                        /* no overwriting it now */
    prog_exec();
    assert(!prog_run_req && !prog_running);
    pm[8 + 2 * 4] = 0x8;                                              /* entry 0x..08: no Thumb bit */
    assert(prog_write(0, pm + 7) == 0 && prog_start(64, psum - 1) == 2);
    assert(prog_write(PROG_SIZE - 32, pm + 7) == 1);                  /* past the end */

    /* a program that owns the inputs: keys and buttons do nothing, its own MIDI still goes out */
    prog_running = prog_own_input = 1;
    uint8_t arp_was = seq_arp_on;
    buttons(1 << BTN_ARP); buttons(0);
    assert(seq_arp_on == arp_was && qt == qh);
    key_row(6, 1 << 3, MS(1000)); key_row(7, 1 << 3, MS(1010));
    assert(qt == qh);                                                 /* key down: no note from the firmware */
    uint8_t kv[25]; keys_level(kv);
    assert(kv[0] == 75);                                              /* but the program sees it */
    key_row(7, 0, MS(1100)); key_row(6, 0, MS(1110));
    midi_queue(0x09 | 0x90u << 8 | 60u << 16 | 100u << 24);
    assert(pop() == (0x09 | 0x90u << 8 | 60u << 16 | 100u << 24));
    /* holding PROG SELECT 2 s resets the keyboard */
    for (int i = 0; i < 3990; i++) ui_button_row(8, 1 << BTN_PROG_SELECT);
    assert(!resets);
    for (int i = 0; i < 20; i++) ui_button_row(8, 1 << BTN_PROG_SELECT);
    assert(resets);
    buttons(0);
    prog_running = prog_own_input = 0;
    while (qt != qh) pop();
    puts("ram programs ok");

    /* host messages to a running program (SysEx 0B): answered by its handler, empty when nobody listens */
    uint8_t hm[] = {0xF0, 0x7D, 0x4D, 0x4B, 0x0B, 5, 6, 0xF7};
    host_send(hm, sizeof hm);
    assert(host_reply(pr) == 6 && pr[4] == 0x4B);                    /* no program: empty answer */
    prog_running = 1;
    on_message = test_echo;
    host_send(hm, sizeof hm);
    assert(host_reply(pr) == 9 && pr[5] == 2 && pr[6] == 6 && pr[7] == 0x7F && pr[8] == 0xF7);
    prog_running = 0, on_message = 0;
    puts("program messages ok");
    return 0;
}
