/* Front-panel inputs as the chip sees them (pin map: firmware/NOTES.md).
 * Matrix: drive rows PC0-7, PB8, PB9 (active low), sense PC8-15 (0 = closed). Each key has two contacts:
 * a press with velocity v closes contact 1, then contact 2 after vel_us(v); a release opens 2, then 1.
 * Analog (read by adc.c through analog_in): PA0/PA1 knob wipers A/B, PA2 pads (all three through 8:1 muxes
 * selected by PB0-2), PA3/PA4 joystick X/Y. Pedal: PA5. */
#include "periph.h"
#include <math.h>

/* ---- matrix ---- */
static uint8_t closed[10];  /* closed[row] bit b = switch at (row, PC(8+b)) */

static void contact(int row, int bit, int on) {
    if (on) closed[row] |= 1u << bit;
    else closed[row] &= ~(1u << bit);
}

static uint16_t portc_in(void *ctx, int port) {
    (void)ctx, (void)port;
    uint16_t c = gpio_out(2), b = gpio_out(1);
    uint8_t sense = 0;
    for (int r = 0; r < 10; r++)
        if (!((r < 8 ? c : b) >> r & 1)) sense |= closed[r];  /* rows 8/9 are PB8/PB9 */
    return (uint16_t)~(sense << 8);
}

/* key k (0 = lowest C): n = 27 - k, first contact on row 2*(n/8), second on the next row, sense bit n % 8 */
static void key_contact(int k, int which, int on) { int n = 27 - k; contact(2 * (n / 8) + which, n % 8, on); }

/* Inverse of the custom firmware's velocity() (fw/main.c): contact time in us for velocity v, aimed at the middle
 * of the interval its integer maths maps to v. Both firmwares time the contacts at their scan rate (custom: 500 us
 * per row pair), so where the curve is steep (2-5 ms, ~110 us per step) the velocity read back is within a few
 * steps of v. Stock has its own curve (e.g. 100 -> 83, 30 -> 8); it gets the same times. */
static uint32_t vel_us(int v) {
    static const uint32_t t[] = {2000, 5000, 10000, 20000, 40000, 80000, 150000};
    static const uint8_t vv[] = {127, 100, 75, 50, 28, 10, 1};
    if (v >= 127) return 1500;
    for (int i = 1; i < 7; i++)
        if (v >= vv[i]) return t[i - 1] + (t[i] - t[i - 1]) * (2u * (vv[i - 1] - v) + 1) / (2u * (vv[i - 1] - vv[i]));
    return 150000;
}
#define RELEASE_US 4000  /* contact 2 opens, then contact 1 this much later */

static uint64_t due_c2[25], due_off[25];  /* pending: close contact 2 / open contact 1 at this cycle (0 = none) */
static void key_ev(void *ctx);
static void key_arm(void) {
    uint64_t m = 0;
    for (int k = 0; k < 25; k++) {
        if (due_c2[k] && (!m || due_c2[k] < m)) m = due_c2[k];
        if (due_off[k] && (!m || due_off[k] < m)) m = due_off[k];
    }
    if (m) ev_at(m, key_ev, 0);
    else ev_cancel(key_ev, 0);
}
static void key_ev(void *ctx) {
    (void)ctx;
    for (int k = 0; k < 25; k++) {
        if (due_c2[k] && due_c2[k] <= now()) due_c2[k] = 0, key_contact(k, 1, 1);
        if (due_off[k] && due_off[k] <= now()) due_off[k] = 0, key_contact(k, 0, 0);
    }
    key_arm();
}

void input_key(int k, int v) {
    if (v) {
        due_off[k] = 0;
        key_contact(k, 0, 1);
        due_c2[k] = now() + us_to_cycles(vel_us(v));
    } else {
        due_c2[k] = 0;
        key_contact(k, 1, 0);
        due_off[k] = now() + us_to_cycles(RELEASE_US);
    }
    key_arm();
}

void input_button(int b, int down) { contact(8 + b / 8, b % 8, down); }

/* ---- analog ---- */
static const uint8_t item_at[8] = {1, 2, 3, 0, 7, 4, 6, 5};  /* mux position -> knob/pad index ([3,0,1,2,5,7,6,4]^-1) */

/* Endless knobs: two triangle-wave wipers 90 degrees apart, A = tri(angle), B = tri(angle + 90), the convention
 * fw/analog.c decodes as clockwise = up (checked on hardware) and fw/test_keys.c uses. A turn takes time: the
 * firmware rejects jumps (custom: > 1500 counts between samples), so the shaft moves at KNOB_DEG_MS. */
#define KNOB_DEG_MS 0.36  /* one turn per second */
static double kn_from[8], kn_to[8];
static uint64_t kn_t0[8];
static double knob_angle(int i) {
    double d = kn_to[i] - kn_from[i], m = (double)(now() - kn_t0[i]) * KNOB_DEG_MS / (F_CPU / 1000.0);
    return fabs(d) <= m ? kn_to[i] : kn_from[i] + (d > 0 ? m : -m);
}
static uint16_t tri(double deg) {
    double x = fmod(deg, 360.0);
    if (x < 0) x += 360;
    return (uint16_t)(x < 180 ? x / 180 * 4095 : (360 - x) / 180 * 4095);
}
void input_knob(int i, int deg) { kn_from[i] = knob_angle(i), kn_to[i] += deg, kn_t0[i] = now(); }

/* Pads: force 1-127 maps onto the custom firmware's hit range (PAD_ON 450, the stock threshold too, .. PAD_MAX
 * 3800), so its velocity = force with the linear curve. shortcut: no rise time or noise; a hit is a step. */
static uint16_t pad_lvl[8];
void input_pad(int p, int f) { pad_lvl[p] = f ? (uint16_t)(450 + (f - 1) * 3350 / 126) : 0; }

/* Joystick: centre 2048, +-1900 at the edges; right / up read higher (custom: right = pitch up, on hardware). */
static uint16_t joy[2] = {2048, 2048};
void input_joystick(int x, int y) { joy[0] = (uint16_t)(2048 + x * 1900 / 1000), joy[1] = (uint16_t)(2048 + y * 1900 / 1000); }

uint16_t analog_in(int ch) {
    int item = item_at[gpio_out(1) & 7];
    switch (ch) {
    case 0: return tri(knob_angle(item));
    case 1: return tri(knob_angle(item) + 90);
    case 2: return pad_lvl[item];
    case 3: return joy[0];
    case 4: return joy[1];
    case 16: return 1750;  /* temperature sensor, ~25 C */
    case 17: return 1490;  /* internal reference 1.2 V of 3.3 V */
    }
    return 0;
}

/* Pedal on PA5 (NOTES: "probably"). Guess: a normally-open pedal shorts the pin to ground, so down = low. */
static int pedal;
void input_pedal(int down) { pedal = down; }
static uint16_t porta_in(void *ctx, int port) { (void)ctx, (void)port; return (uint16_t)~(pedal << 5); }

static void inputs_reset(void *ctx) { (void)ctx; key_arm(); }  /* the core dropped our event; held inputs stay */

void inputs_init(void) {
    for (int i = 0; i < 8; i++) kn_from[i] = kn_to[i] = 30;
    gpio_input(2, portc_in, 0);
    gpio_input(0, porta_in, 0);
    on_reset(inputs_reset, 0);
}
