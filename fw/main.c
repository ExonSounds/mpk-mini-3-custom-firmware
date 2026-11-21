/* MPK mini 3 custom firmware: USB-MIDI device. Keys send notes with velocity (channel, transpose and curve from
 * settings, PROG SELECT opens the menu: menu.c); pads, knobs and
 * joystick are in analog.c. Screen: three numbers for the last event (key: note, velocity, contact time in ms;
 * pad: note, velocity, peak/10; knob: CC, value, knob #; joystick: value MSB/CC, LSB/value); square = USB up. */
#include "analog.h"
#include "board.h"
#include "cfg.h"
#include "menu.h"
#include "pong.h"
#include "prog.h"
#include "seq.h"
#include "settings.h"
#include "ui.h"
#include "usb_midi.h"
#include "views.h"

/* ---- MIDI event queue: SysTick (producer) -> main loop (consumer) ---- */
#define QN 256  /* power of two; a settings reply alone is ~20 packets */
static volatile uint32_t q[QN];
static volatile uint8_t qh, qt;

void midi_queue(uint32_t pkt) {
    uint8_t n = (qh + 1) & (QN - 1);
    if (n != qt) { q[qh] = pkt; qh = n; }  /* full: drop (nobody is reading the port) */
}
void midi_push(uint32_t pkt) { if (!prog_own_input) midi_queue(pkt); }  /* a RAM program can take the inputs */

volatile uint32_t ticks;                   /* also read by RAM programs (prog.c) */
static volatile uint32_t last_use;  /* SysTick count (20 kHz) and when anything was last used */
void ui_activity(void) { last_use = ticks; }

/* From the USB interrupt (same priority as SysTick, so it never interrupts the sequencer). */
void usb_midi_received(uint32_t pkt) {
    if ((pkt & 0xF) == 0xF) seq_realtime(pkt >> 8 & 0xFF);  /* CIN 0xF: single byte, e.g. clock F8 / start FA */
    else cfg_rx(pkt);                                       /* SysEx from the configuration GUI */
}

static volatile uint8_t shown_dirty = 1;
static volatile uint8_t view = V_HOME;
static volatile uint32_t view_until;  /* MSG / TEMPO go back to HOME after this (compared signed: see idle check) */
static char msg_title[22], msg_value[12];

void show_view(int v) {
    view = v, shown_dirty = 1;
    view_until = ticks + (v == V_TEMPO ? 3 * 20000u : 30000u);  /* tempo 3 s after the last tap, messages 1.5 s */
    ui_activity();
}

void show_msg(const char *title, const char *value) {
    unsigned i;
    for (i = 0; title[i] && i < sizeof msg_title - 1; i++) msg_title[i] = title[i];
    msg_title[i] = 0;
    for (i = 0; value[i] && i < sizeof msg_value - 1; i++) msg_value[i] = value[i];
    msg_value[i] = 0;
    show_view(V_MSG);
}

/* ---- Keys ---- */
#define BASE_NOTE 48  /* lowest key = C3, like stock's default octave */
enum { IDLE, ARMED, ON };
static uint8_t kstate[25], knote[25];  /* knote: note sent at note-on, so octave changes can't strand a note */
static uint8_t kvia[25];               /* 1 = this key went to the arpeggiator */
static uint8_t kvel[25];               /* velocity it was struck with, for the keyboard view */
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
                show_view(V_KEYS);
                if (kvia[k]) arp_key(knote[k], 0, 0);
                else midi_push(0x08 | (0x80u | S.keys_ch) << 8 | (uint32_t)knote[k] << 16 | 0x40u << 24);
            }
        } else if (kstate[k] == ARMED && c) {  /* second contact: note on */
            uint32_t us = (now - kt1[k]) / (F_CPU / 1000000);
            uint8_t vel = ui_full_level ? 127 : curve(velocity(us), S.key_curve);  /* FULL LEVEL: keys too */
            kvel[k] = vel;
            kstate[k] = ON;
            int n = BASE_NOTE + 12 * ui_octave + (S.transpose - 12) + k;
            knote[k] = n < 0 ? 0 : n > 127 ? 127 : n;
            kvia[k] = seq_arp_on;
            if (kvia[k]) arp_key(knote[k], vel, 1);
            else midi_push(0x09 | (0x90u | S.keys_ch) << 8 | (uint32_t)knote[k] << 16 | (uint32_t)vel << 24);
            show_view(V_KEYS);
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

void keys_level(uint8_t vel[25]) { for (int k = 0; k < 25; k++) vel[k] = kstate[k] == ON ? kvel[k] : 0; }

static int differs(const void *a, const void *b, unsigned n) {  /* no libc memcmp on the target */
    const uint8_t *x = a, *y = b;
    while (n--) if (*x++ != *y++) return 1;
    return 0;
}

static void send_queued(void) {
    uint32_t buf[16];
    int n = 0;
    for (uint8_t i = qt; i != qh && n < 16; i = (i + 1) & (QN - 1)) buf[n++] = q[i];
    if (n && usb_midi_send(buf, n)) qt = (qt + n) & (QN - 1);
}

void midi_flush(void) { send_queued(); }
void oled_flush_hook(void) { send_queued(); }  /* keeps MIDI timing tight while a frame is drawn */

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

    int was_configured = -1, screen_on = 1, in_pong = 0, img_inv = 0;  /* in_pong: 1 ping-pong, 2 idle image shown */
    uint32_t pong_due = 0, last_draw = 0;
    home_t drawn_home = {0};
    int drawn_beat = 0;
    for (;;) {
        send_queued();
        cfg_poll();
        if (prog_run_req) {  /* RAM program uploaded and checked: it has the keyboard until it returns */
            prog_exec();
            ui_activity();
            shown_dirty = 1, menu_dirty = 1, in_pong = 0, view = V_HOME;
            continue;
        }

        if (save_req) {  /* menu closed: store settings (interrupts are off for the ~20-40 ms erase) */
            save_req = 0;
            int ok = settings_save();
            fb_clear();
            text(0, 9, ok ? "SAVED" : "SAVE FAILED", 2);
            oled_flush();
            shown_dirty = 0;
            continue;
        }
        /* after IDLE AFTER with nothing used: ping-pong or screen fully off; any input brings the normal screen back */
        static const uint8_t off_secs[5] = {5, 10, 30, 60, 0};
        /* signed: the SysTick can update last_use after `ticks` was read here, making the difference negative
         * (= just used). Unsigned, that read as ~4 billion ticks idle and flashed a ping-pong frame mid-playing. */
        int idle = !menu_on && off_secs[S.screen_off] &&
                   (int32_t)(ticks - last_use) >= (int32_t)(off_secs[S.screen_off] * 20000u);
        int want_on = !idle || S.idle_screen != IDLE_OFF;
        if (want_on != screen_on) {
            screen_on = want_on;
            oled_power(want_on);
            ui_leds_off = !want_on;  /* idle screen OFF darkens the LEDs too */
            if (want_on) shown_dirty = 1, menu_dirty = 1;
        }
        if (!screen_on) continue;
        if (idle && S.idle_screen == IDLE_IMAGE) {  /* static image, inverted every minute against burn-in */
            int inv = (int32_t)(ticks - last_use) / (60 * 20000) & 1;
            if (in_pong != 2 || inv != img_inv || image_dirty) {
                in_pong = 2, img_inv = inv, image_dirty = 0;
                draw_idle_image(idle_image_set ? idle_image : 0, inv);
                oled_flush();
            }
            continue;
        }
        if (idle) {
            if (in_pong != 1) in_pong = 1, pong_reset(), pong_due = ticks;
            if ((int32_t)(ticks - pong_due) >= 0) {  /* ~30 frames per second */
                pong_due += 660;
                pong_step();
                pong_draw();
                oled_flush();
            }
            continue;
        }
        if (in_pong) in_pong = 0, shown_dirty = 1;
        if (menu_on) {
            if (menu_dirty) menu_dirty = 0, ui_activity(), menu_draw();
            continue;
        }
        int cfg = usb_configured();
        if ((view == V_MSG || view == V_TEMPO) && (int32_t)(ticks - view_until) >= 0) view = V_HOME, shown_dirty = 1;
        home_t home = {.octave = ui_octave, .bank = ui_bank, .pad_mode = ui_pad_mode, .arp = seq_arp_on,
                       .repeat = seq_repeat_on, .full = ui_full_level, .ext = S.clock_ext, .usb = cfg,
                       .bpm = (uint8_t)seq_bpm()};
        int beat = seq_beat_led;
        if ((view == V_HOME && differs(&home, &drawn_home, sizeof home)) ||
            (view == V_TEMPO && (beat != drawn_beat || home.bpm != drawn_home.bpm)))
            shown_dirty = 1;
        /* views redraw as things change, at most every 20 ms so the screen never holds up MIDI */
        if ((shown_dirty && ticks - last_draw >= 400) || cfg != was_configured) {
            shown_dirty = 0, was_configured = cfg, last_draw = ticks;
            if (view == V_KEYS) {
                uint8_t level[25];
                keys_level(level);
                draw_keys(level);
            } else if (view == V_PADS) {
                uint8_t level[8];
                pads_level(level);
                draw_pads(level, ui_bank);
            } else if (view == V_JOY) {
                int x, y;
                joy_state(&x, &y);
                draw_joystick(x, y);
            } else if (view == V_KNOBS) {
                uint8_t pos[8], value[8], rel;
                knob_state(pos, value, &rel);
                draw_knobs(pos, value, rel);
            } else if (view == V_MSG) {
                draw_msg(msg_title, msg_value);
            } else if (view == V_TEMPO) {
                draw_tempo(home.bpm, home.ext, beat);
            } else {
                draw_home(&home);
            }
            drawn_home = home, drawn_beat = beat;
            oled_flush();
        }
    }
}
