/* 24 LEDs on 3 chained 8-bit shift registers: data PB5, clock PB3 (rising edge), latch PB4 (rising edge),
 * PB7 high = outputs on (both firmwares drive it high). Bits come bit-banged (custom led_out) or as SPI1 bytes
 * (stock, DMA1 ch3), MSB first, LEDs 0-7 first. Outputs are active low: a latched 0 = LED on.
 * Brightness = fraction of time on (software PWM: custom 6.4 ms period) over a 64 ms window, sent on change. */
#include "periph.h"

#define PIN_SCK (1u << 3)
#define PIN_LAT (1u << 4)
#define PIN_SDA (1u << 5)
#define PIN_EN  (1u << 7)
#define WINDOW (F_CPU * 64 / 1000)

static uint32_t sreg, out = 0xFFFFFF;  /* shift register chain, latched outputs */
static int en;
static uint64_t t_last, t_win, on_time[24];
static uint8_t shown[24];

/* After 24 clocks the first bit sent (LED 7, MSB of byte 0) is in the last stage (bit 23): LED n -> this bit */
static int bitpos(int n) { return 16 - 8 * (n / 8) + n % 8; }

static void account(void) {
    uint64_t t = now();
    for (int n = 0; n < 24; n++)
        if (en && !(out >> bitpos(n) & 1)) on_time[n] += t - t_last;
    t_last = t;
}

static void window(void *ctx) {
    (void)ctx;
    account();
    uint64_t w = now() - t_win;
    uint8_t lvl[24];
    int change = 0;
    for (int n = 0; n < 24; n++) {
        lvl[n] = w ? (uint8_t)(on_time[n] * 255 / w) : 0;
        int d = lvl[n] - shown[n];
        if (d > 2 || d < -2 || ((lvl[n] == 0 || lvl[n] == 255) && lvl[n] != shown[n])) change = 1;  /* PWM jitter */
        on_time[n] = 0;
    }
    t_win = now();
    static int sent;
    if (change || !sent) {
        sent = 1;
        for (int n = 0; n < 24; n++) shown[n] = lvl[n];
        panel_leds(shown);
    }
    ev_at(now() + WINDOW, window, 0);
}

static void shift_bit(int b) { sreg = (sreg << 1 | (b & 1)) & 0xFFFFFF; }

void leds_spi_byte(uint8_t b) { for (int i = 7; i >= 0; i--) shift_bit(b >> i); }

static void pins(void *ctx, int port, uint16_t old, uint16_t nw) {
    (void)ctx, (void)port;
    uint16_t rise = nw & ~old;
    if (rise & PIN_SCK) shift_bit(nw >> 5);
    if ((rise & PIN_LAT) || ((nw ^ old) & PIN_EN)) {
        account();
        if (rise & PIN_LAT) out = sreg;
        en = (nw & PIN_EN) != 0;
    }
}

static void leds_reset(void *ctx) {  /* the registers keep their state (no power cycle); the core dropped our event */
    (void)ctx;
    account();
    en = 0;  /* PB7 floats until the firmware drives it */
    ev_at(now() + WINDOW, window, 0);
}

void leds_init(void) {
    gpio_watch(1, pins, 0);
    on_reset(leds_reset, 0);
}
