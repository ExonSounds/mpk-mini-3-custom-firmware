#include "ui.h"
#include "analog.h"
#include "board.h"
#include "menu.h"
#include "prog.h"
#include "prog_api.h"
#include "seq.h"
#include "settings.h"

volatile int8_t ui_octave;
volatile uint8_t ui_bank, ui_pad_mode, ui_full_level;

/* ---- LEDs: software PWM, 32 levels. Like stock: button LEDs at 4/32 (it caps them there), bank and pads full. ---- */
#define LED_BANK_A 0
#define LED_BANK_B 1
#define LED_PROG_CHANGE 2
#define LED_CC 3
#define LED_PROG_SELECT 4
#define LED_PAD0 8
#define LED_ARP 16
#define LED_TAP_TEMPO 17
#define LED_OCT_UP 18
#define LED_OCT_DOWN 19
#define LED_FULL_LEVEL 20
#define LED_NOTE_REPEAT 21
#define DIM 4
#define FULL 32

volatile uint8_t ui_led_level[24];

static void update_leds(void) {
    ui_led_level[LED_BANK_A] = ui_bank ? 0 : FULL;
    ui_led_level[LED_BANK_B] = ui_bank ? FULL : 0;
    ui_led_level[LED_CC] = ui_pad_mode == PADS_CC ? DIM : 0;
    ui_led_level[LED_PROG_CHANGE] = ui_pad_mode == PADS_PROG ? DIM : 0;
    ui_led_level[LED_OCT_UP] = ui_octave > 0 ? DIM : 0;
    ui_led_level[LED_OCT_DOWN] = ui_octave < 0 ? DIM : 0;
    ui_led_level[LED_FULL_LEVEL] = ui_full_level ? DIM : 0;
    ui_led_level[LED_PROG_SELECT] = menu_on ? DIM : 0;
    ui_led_level[LED_ARP] = seq_arp_on ? DIM : 0;
    ui_led_level[LED_NOTE_REPEAT] = seq_repeat_on ? DIM : 0;
}

void ui_pad_led(int pad, int on) { if (!prog_own_input) ui_led_level[LED_PAD0 + pad] = on ? FULL : 0; }

void ui_leds_reset(void) {  /* after a RAM program: its LEDs off, the firmware's back */
    for (int i = 0; i < 24; i++) ui_led_level[i] = 0;
    update_leds();
}
_Static_assert(LED_PAD0 == MPK_LED_PAD0 && LED_NOTE_REPEAT == MPK_LED_NOTE_REPEAT &&
               LED_PROG_SELECT == MPK_LED_PROG_SELECT && BTN_NOTE_REPEAT == MPK_BTN_NOTE_REPEAT &&
               BTN_PROG_SELECT == MPK_BTN_PROG_SELECT, "prog_api.h numbers");

volatile uint8_t ui_leds_off;

void ui_led_tick(void) {  /* one PWM slot every 4 ticks: 32 slots = 6.4 ms period */
    static uint8_t div, slot, started;
    static uint32_t shown = ~0u;
    if (!started) { update_leds(); started = 1; }
    if (++div < 4) return;
    div = 0;
    slot = (slot + 1) & 31;
    if (!prog_own_input) ui_led_level[LED_TAP_TEMPO] = seq_beat_led ? DIM : 0;  /* blinks on the beat */
    uint32_t bits = 0;
    if (!ui_leds_off) for (int i = 0; i < 24; i++) if (ui_led_level[i] > slot) bits |= 1u << i;
    if (bits != shown) { led_out(bits); shown = bits; }
}

/* ---- Buttons ---- */
static void press(int b, uint16_t held) {
    ui_activity();
    if (b == BTN_PROG_SELECT) { menu_toggle(); update_leds(); return; }
    if (menu_on) {  /* in the menu only OCT-/OCT+ do something: previous/next setting */
        if (b == BTN_OCT_UP) menu_move(1);
        else if (b == BTN_OCT_DOWN) menu_move(-1);
        return;
    }
    switch (b) {
    case BTN_OCT_UP:
    case BTN_OCT_DOWN: {
        int other = b == BTN_OCT_UP ? BTN_OCT_DOWN : BTN_OCT_UP;
        if (held >> other & 1) ui_octave = 0;  /* both together: back to normal */
        else if (b == BTN_OCT_UP && ui_octave < 4) ui_octave++;
        else if (b == BTN_OCT_DOWN && ui_octave > -4) ui_octave--;
        char v[3] = {ui_octave > 0 ? '+' : ui_octave < 0 ? '-' : ' ', (char)('0' + (ui_octave < 0 ? -ui_octave : ui_octave)), 0};
        show_msg("OCTAVE", ui_octave ? v : "0");
        break;
    }
    case BTN_PAD_BANK: ui_bank ^= 1; show_view(V_PADS); break;
    case BTN_CC:
    case BTN_PROG_CHANGE: {
        int m = b == BTN_CC ? PADS_CC : PADS_PROG;
        ui_pad_mode = ui_pad_mode == m ? PADS_NOTE : m;
        static const char *const names[3] = {"NOTES", "CC", "PROGRAM"};
        show_msg("PAD MODE", names[ui_pad_mode]);
        break;
    }
    case BTN_FULL_LEVEL: ui_full_level ^= 1; show_msg("FULL LEVEL", ui_full_level ? "ON" : "OFF"); break;
    case BTN_ARP: seq_arp_toggle(); show_msg("ARPEGGIATOR", seq_arp_on ? "ON" : "OFF"); break;
    case BTN_NOTE_REPEAT: seq_repeat_on ^= 1; show_msg("NOTE REPEAT", seq_repeat_on ? "ON" : "OFF"); break;
    case BTN_TAP_TEMPO: seq_tap(); show_view(V_TEMPO); break;  /* BPM screen, updates with every tap */
    default: return;
    }
    update_leds();
}

/* Debounce: a button changes state after DEB consecutive equal reads of its row (one read per 0.5 ms). */
#define DEB 10
static uint8_t cnt[16];
volatile uint16_t ui_held;

void ui_button_row(int row, uint8_t closed) {
    static uint16_t escape;  /* PROG SELECT held while a RAM program runs: reset after 2 s (row 8 comes at 2 kHz) */
    for (int bit = 0; bit < 8; bit++) {
        int b = bit + 8 * (row - 8);
        if (closed >> bit & 1) {
            if (cnt[b] < DEB && ++cnt[b] == DEB && !(ui_held >> b & 1)) {
                ui_held |= 1u << b;
                if (!prog_own_input) press(b, ui_held);
            }
        } else if (cnt[b] > 0 && --cnt[b] == 0) {
            ui_held &= ~(1u << b);
        }
    }
    if (row == 8) escape = prog_running && (ui_held & 1u << BTN_PROG_SELECT) ? escape + 1 : 0;
    if (escape >= 4000) system_reset();
}
