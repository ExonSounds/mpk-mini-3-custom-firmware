/* SSD1306 OLED controller (128 x 64 GDDRAM, 8 pages) behind a 128x32 glass. Bytes come from the custom fw's
 * bit-banged pins (PB13 SCK, PB15 SDA, PB12 CS, PB14 D/C, PB10 RESET: SPI mode 0, MSB first, D/C sampled with
 * the 8th bit) or from stock's SPI2 (+ DMA1 ch5), with CS and D/C still read from the pins.
 *
 * Which half of the RAM is visible: both firmwares set multiplex 64 (A8 3F), offset 0 (D3 00), start line 0 and
 * remapped COM scan (C8). Scanning COM63..COM0 then puts RAM row r on COM(63 - r). The glass has 32 rows wired
 * to COM0-31 (as on a normal 128x32 module, where A8 1F + C8 shows RAM row 0 on the top row, i.e. top = COM31),
 * so it shows RAM rows 32-63 = pages 4-7, top row = page 4 bit 0. That is NOTES.md's "frames pinned to pages 0-3
 * are invisible": the custom fw writes its frame to pages 0-7 (twice) so pages 4-7 hold it; stock sends 512 data
 * bytes per frame in horizontal mode without resetting the page pointer, so its frames alternate between pages
 * 0-3 (hidden) and 4-7 (shown) and the glass always has the latest-but-one or latest frame. The general rule
 * below (glass row y = COM 31 - y, start line, offset, mux, both remaps) reduces to that for these settings.
 * shortcut: display offset is applied like the start line (exact only for D3 00, which both firmwares use);
 * scrolling (26/27/29/2A/2F), fade and zoom are accepted but not shown. */
#include "periph.h"
#include <string.h>

#define PIN_RES (1u << 10)
#define PIN_CS  (1u << 12)
#define PIN_SCK (1u << 13)
#define PIN_DC  (1u << 14)
#define PIN_SDA (1u << 15)

static uint8_t ram[8][128];
static struct {
    int mode, col, page, c0, c1, p0, p1, start, offset, mux, seg_remap, com_remap, contrast, on, invert, entire, pump;
    uint8_t cmd[8];
    int ncmd, need;
} D;
static int dirty, pending;  /* pending: a show() event is scheduled */

static void ssd_reset(void) {
    memset(&D, 0, sizeof D);
    D.mode = 2, D.c1 = 127, D.p1 = 7, D.mux = 64, D.contrast = 0x7F;
    dirty = 1;
}

static int args(uint8_t c) {  /* parameter bytes after command c */
    switch (c) {
    case 0x20: case 0x81: case 0x8D: case 0xA8: case 0xD3: case 0xD5: case 0xD6: case 0xD9: case 0xDA: case 0xDB: return 1;
    case 0x21: case 0x22: case 0xA3: return 2;
    case 0x29: case 0x2A: return 5;
    case 0x26: case 0x27: return 6;
    }
    return 0;
}

static void command(const uint8_t *c) {
    uint8_t k = c[0];
    if (k < 0x10) D.col = (D.col & 0xF0) | k;               /* page mode column, low nibble */
    else if (k < 0x20) D.col = (D.col & 0x0F) | (k & 7) << 4; /* high nibble */
    else if (k >= 0x40 && k < 0x80) D.start = k & 63;
    else if (k >= 0xB0 && k < 0xB8) D.page = k & 7;
    else switch (k) {
    case 0x20: D.mode = c[1] & 3; break;
    case 0x21: D.c0 = c[1] & 127, D.c1 = c[2] & 127, D.col = D.c0; break;
    case 0x22: D.p0 = c[1] & 7, D.p1 = c[2] & 7, D.page = D.p0; break;
    case 0x81: D.contrast = c[1]; break;
    case 0x8D: D.pump = c[1] >> 2 & 1; break;
    case 0xA0: case 0xA1: D.seg_remap = k & 1; break;
    case 0xA4: case 0xA5: D.entire = k & 1; break;
    case 0xA6: case 0xA7: D.invert = k & 1; break;
    case 0xA8: if ((c[1] & 63) >= 15) D.mux = (c[1] & 63) + 1; break;
    case 0xAE: case 0xAF: D.on = k & 1; break;
    case 0xC0: case 0xC8: D.com_remap = k >> 3 & 1; break;
    case 0xD3: D.offset = c[1] & 63; break;
    }
    dirty = 1;
}

static void data(uint8_t b) {
    ram[D.page][D.col] = b;
    dirty = 1;
    if (D.mode == 0) {         /* horizontal: column, then page, inside the window */
        if (++D.col > D.c1) D.col = D.c0, D.page = D.page >= D.p1 ? D.p0 : D.page + 1;
    } else if (D.mode == 1) {  /* vertical */
        if (++D.page > D.p1) D.page = D.p0, D.col = D.col >= D.c1 ? D.c0 : D.col + 1;
    } else if (++D.col > 127) D.col = 0;  /* page mode: stays on the page */
}

static void byte_in(uint8_t b, int dc) {
    if (dc) { D.ncmd = 0; data(b); return; }
    if (!D.ncmd) D.need = args(b);
    D.cmd[D.ncmd++] = b;
    if (D.ncmd > D.need) { command(D.cmd); D.ncmd = 0; }
}

/* ---- the picture on the glass, at most every 10 ms ---- */
static void show(void *ctx) {
    (void)ctx;
    static int waits;  /* mid-transfer (CS low): wait for the end so frames don't tear, but not for ever */
    if (!(gpio_out(1) & PIN_CS) && ++waits < 40) { ev_at(now() + F_CPU / 2000, show, 0); return; }
    waits = 0;
    uint8_t fb[512] = {0};
    for (int y = 0; y < 32; y++) {
        int com = 31 - y, j = D.com_remap ? D.mux - 1 - com : com;  /* scan position of that COM line */
        if (j < 0 || j >= D.mux) continue;
        int row = (D.start + D.offset + j) & 63;
        for (int x = 0; x < 128; x++) {
            int lit = D.entire || (ram[row >> 3][D.seg_remap ? x : 127 - x] >> (row & 7) & 1);
            if (lit) fb[(y >> 3) * 128 + x] |= 1u << (y & 7);
        }
    }
    dirty = 0, pending = 0;
    panel_frame(fb, D.on && D.pump, D.contrast, D.invert && !D.entire);
}
static uint64_t last_show;

static void mark(void) {
    if (!dirty || pending) return;
    uint64_t t = last_show + F_CPU / 100;
    if (t < now()) t = now();
    last_show = t, pending = 1;
    ev_at(t, show, 0);
}

void oled_spi_byte(uint8_t b) {
    uint16_t p = gpio_out(1);
    if (!(p & PIN_RES) || (p & PIN_CS)) return;
    byte_in(b, p & PIN_DC ? 1 : 0);
    mark();
}

/* ---- bit-banged pins ---- */
static int nbits;
static uint8_t sh;
static void pins(void *ctx, int port, uint16_t old, uint16_t nw) {
    (void)ctx, (void)port;
    if (!(nw & PIN_RES)) {
        if (old & PIN_RES) ssd_reset(), mark();
        return;
    }
    if (nw & PIN_CS) { nbits = 0; return; }
    if ((nw & ~old) & PIN_SCK) {
        sh = (uint8_t)(sh << 1 | (nw >> 15 & 1));
        if (++nbits == 8) nbits = 0, byte_in(sh, nw & PIN_DC ? 1 : 0), mark();
    }
}

static void oled_reset(void *ctx) { (void)ctx; nbits = 0, last_show = 0, pending = 0, dirty = 1; mark(); }  /* core dropped our event */

void oled_init(void) {
    ssd_reset();
    gpio_watch(1, pins, 0);
    on_reset(oled_reset, 0);
}
