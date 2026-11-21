/* MPK mini 3 custom firmware: USB-MIDI device. Keys send notes with velocity (channel, transpose and curve from
 * settings, PROG SELECT opens the menu: menu.c); pads, knobs and
 * joystick are in analog.c. Screen: three numbers for the last event (key: note, velocity, contact time in ms;
 * pad: note, velocity, peak/10; knob: CC, value, knob #; joystick: value MSB/CC, LSB/value); square = USB up. */
#include "analog.h"
#include "board.h"
#include "menu.h"
#include "seq.h"
#include "settings.h"
#include "ui.h"
#include "usb_midi.h"

/* ---- MIDI event queue: SysTick (producer) -> main loop (consumer) ---- */
static volatile uint32_t q[64];
static volatile uint8_t qh, qt;

void midi_push(uint32_t pkt) {
    uint8_t n = (qh + 1) & 63;
    if (n != qt) { q[qh] = pkt; qh = n; }  /* full: drop (nobody is reading the port) */
}

static volatile uint32_t ticks, last_use;  /* SysTick count (20 kHz) and when anything was last used */
void ui_activity(void) { last_use = ticks; }

/* From the USB interrupt (same priority as SysTick, so it never interrupts the sequencer). */
void usb_midi_received(uint32_t pkt) {
    if ((pkt & 0xF) == 0xF) seq_realtime(pkt >> 8 & 0xFF);  /* CIN 0xF: single byte, e.g. clock F8 / start FA */
}

static volatile int16_t shown[3];
static volatile uint8_t shown_dirty = 1;
void show3(int a, int b, int c) { shown[0] = a, shown[1] = b, shown[2] = c, shown_dirty = 1; ui_activity(); }

/* ---- Keys ---- */
#define BASE_NOTE 48  /* lowest key = C3, like stock's default octave */
enum { IDLE, ARMED, ON };
static uint8_t kstate[25], knote[25];  /* knote: note sent at note-on, so octave changes can't strand a note */
static uint8_t kvia[25];               /* 1 = this key went to the arpeggiator */
static uint32_t kt1[25];

/* Velocity from the time between first and second contact.
 * shortcut: curve points are a guess, not from stock (its table units are unknown); tune with the ms readout. */
static uint8_t velocity(uint32_t us) {
    static const uint32_t t[] = {2000, 5000, 10000, 20000, 40000, 80000, 150000};
    static const uint8_t v[] = {127, 100, 75, 50, 28, 10, 1};
    if (us <= t[0]) return v[0];
    for (int i = 1; i < 7; i++)
        if (us < t[i]) return v[i - 1] - (uint32_t)(v[i - 1] - v[i]) * (us - t[i - 1]) / (t[i] - t[i - 1]);
    return 1;
}

static void key_row(int row, uint8_t closed, uint32_t now) {
    for (int b = 0; b < 8; b++) {
        int n = b + 8 * (row / 2), k = 27 - n;  /* see firmware/NOTES.md */
        if (k < 0 || k > 24) continue;
        int c = closed >> b & 1;
        if (!(row & 1)) {  /* first contact */
            if (kstate[k] == IDLE && c) kstate[k] = ARMED, kt1[k] = now;
            else if (kstate[k] == ARMED && !c) kstate[k] = IDLE;
            else if (kstate[k] == ON && !c) {
                kstate[k] = IDLE;
                if (kvia[k]) arp_key(knote[k], 0, 0);
                else midi_push(0x08 | (0x80u | S.keys_ch) << 8 | (uint32_t)knote[k] << 16 | 0x40u << 24);
            }
        } else if (kstate[k] == ARMED && c) {  /* second contact: note on */
            uint32_t us = (now - kt1[k]) / (F_CPU / 1000000);
            uint8_t vel = ui_full_level ? 127 : curve(velocity(us), S.key_curve);  /* FULL LEVEL: keys too */
            kstate[k] = ON;
            int n = BASE_NOTE + 12 * ui_octave + (S.transpose - 12) + k;
            knote[k] = n < 0 ? 0 : n > 127 ? 127 : n;
            kvia[k] = seq_arp_on;
            if (kvia[k]) arp_key(knote[k], vel, 1);
            else midi_push(0x09 | (0x90u | S.keys_ch) << 8 | (uint32_t)knote[k] << 16 | (uint32_t)vel << 24);
            show3(knote[k], vel, us > 999000 ? 999 : us / 1000);
        }
    }
}

/* One matrix row per tick at 20 kHz: read the row driven last tick (50 us to settle), then drive the next. */
void SysTick_Handler(void) {
    static int row;
    uint8_t closed = matrix_read();
    uint32_t now = DWT_CYCCNT;
    ticks++;
    if (row < 8) key_row(row, closed, now);
    else ui_button_row(row, closed);
    row = row == 9 ? 0 : row + 1;
    matrix_drive(row);
    analog_tick();
    seq_tick();
    ui_led_tick();
}

static void number(int x, int y, int v, int scale) {
    for (int i = 2; i >= 0; i--, v /= 10) digit(x + i * 4 * scale, y, v % 10, scale);
}

int main(void) {
    board_init();
    settings_load();
    matrix_drive(0);
    analog_init();
    usb_init();
    SYST_RVR = F_CPU / 20000 - 1;
    SYST_CVR = 0;
    SYST_CSR = 7;
    irq_enable();

    int was_configured = -1, screen_on = 1;
    for (;;) {
        uint32_t buf[16];
        int n = 0;
        for (uint8_t i = qt; i != qh && n < 16; i = (i + 1) & 63) buf[n++] = q[i];
        if (n && usb_midi_send(buf, n)) qt = (qt + n) & 63;

        if (save_req) {  /* menu closed: store settings (interrupts are off for the ~20-40 ms erase) */
            save_req = 0;
            int ok = settings_save();
            fb_clear();
            text(0, 9, ok ? "SAVED" : "SAVE FAILED", 2);
            oled_flush();
            shown_dirty = 0;
            continue;
        }
        /* screen fully off after the SCREEN OFF time with nothing used; any input turns it back on */
        static const uint8_t off_secs[5] = {5, 10, 30, 60, 0};
        int want_on = menu_on || !off_secs[S.screen_off] || ticks - last_use < off_secs[S.screen_off] * 20000u;
        if (want_on != screen_on) {
            screen_on = want_on;
            oled_power(want_on);
            if (want_on) shown_dirty = 1, menu_dirty = 1;
        }
        if (!screen_on) continue;
        if (menu_on) {
            if (menu_dirty) menu_dirty = 0, ui_activity(), menu_draw();
            continue;
        }
        int cfg = usb_configured();
        if (shown_dirty || cfg != was_configured) {
            shown_dirty = 0, was_configured = cfg;
            fb_clear();
            number(0, 3, shown[0], 5);
            number(64, 3, shown[1], 3);
            number(104, 3, shown[2], 2);
            if (cfg) rect(120, 26, 6, 6);
            oled_flush();
        }
    }
}
