#include "views.h"
#include "board.h"

static void clear(int x, int y, int w, int h) {
    for (int i = x; i < x + w; i++)
        for (int j = y; j < y + h; j++)
            if (i >= 0 && i < 128 && j >= 0 && j < 32) fb[(j >> 3) * 128 + i] &= ~(1u << (j & 7));
}

/* Ordered (4x4 Bayer) dither: v 1-127 lights 1/16 .. 16/16 of the area, so the fill shows how hard. */
static void dither(int x, int y, int w, int h, int v) {
    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    int lvl = (v * 16 + 126) / 127;
    for (int i = x; i < x + w; i++)
        for (int j = y; j < y + h; j++)
            if (lvl > bayer[j & 3][i & 3]) px(i, j);
}

static void frame(int x, int y, int w, int h) {
    rect(x, y, w, 1), rect(x, y + h - 1, w, 1), rect(x, y, 1, h), rect(x + w - 1, y, 1, h);
}

/* ---- keyboard: 15 white keys (8 px, x 4-123) and 10 black keys; a held key is filled by its velocity ---- */
#define KX 4
#define KW 8
#define BLACK_H 19

void draw_keys(const uint8_t level[25]) {
    static const int8_t white_of[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};  /* semitone -> white index */
    static const int8_t black_after[12] = {-1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1}; /* black: right of white */
    fb_clear();
    rect(KX, 0, 15 * KW + 1, 1), rect(KX, 31, 15 * KW + 1, 1);
    for (int i = 0; i <= 15; i++) rect(KX + i * KW, 0, 1, 32);
    for (int k = 0; k < 25; k++) {
        int s = k % 12, o = k / 12;
        if (white_of[s] >= 0 && level[k]) dither(KX + (o * 7 + white_of[s]) * KW + 2, 2, KW - 3, 28, level[k]);
    }
    for (int k = 0; k < 25; k++) {
        int s = k % 12, o = k / 12;
        if (black_after[s] < 0) continue;
        int cx = KX + (o * 7 + black_after[s] + 1) * KW;  /* on the boundary between two white keys */
        clear(cx - 3, 0, 7, BLACK_H + 1);                 /* a dark gap keeps it apart from a held white key */
        rect(cx - 3, 0, 7, 1);                            /* restore the top edge over the gap */
        frame(cx - 2, 0, 5, BLACK_H);
        if (level[k]) dither(cx - 1, 1, 3, BLACK_H - 2, level[k]);
    }
}

/* ---- pads: 2 rows x 4 like on the keyboard (top 5-8, bottom 1-4), labelled; held = filled by live pressure ---- */
void draw_pads(const uint8_t level[8], int bank) {
    fb_clear();
    for (int i = 0; i < 8; i++) {
        int x = 1 + 32 * (i & 3), y = i < 4 ? 17 : 0;
        if (level[i]) {
            frame(x, y, 30, 15);
            dither(x + 1, y + 1, 28, 13, level[i]);
        } else {
            char label[3] = {bank ? 'B' : 'A', (char)('1' + i), 0};
            frame(x, y, 30, 15);
            text(x + 9, y + 4, label, 1);
        }
    }
}

/* ---- joystick: square with a dotted crosshair and a dot at the stick's position, X / Y in -100..100 ---- */
static void put_pct(int x, int y, int v) {  /* "-42", "0", "100" at scale 1 */
    char s[6], *p = s;
    v = v * 100 / 1000;
    if (v < 0) *p++ = '-', v = -v;
    if (v >= 100) *p++ = '1', *p++ = '0', *p++ = '0';
    else { if (v >= 10) *p++ = '0' + v / 10; *p++ = '0' + v % 10; }
    *p = 0;
    text(x, y, s, 1);
}

void draw_joystick(int x, int y) {
    fb_clear();
    frame(48, 0, 32, 32);
    for (int i = 1; i < 31; i += 2) px(64, i), px(48 + i, 16);
    rect(64 + x * 14 / 1000 - 1, 16 - y * 14 / 1000 - 1, 3, 3);
    text(14, 2, "X", 2);
    put_pct(8, 21, x);
    text(102, 2, "Y", 2);
    put_pct(96, 21, y);
}

/* ---- dials: 8 across (16 px each), pointer from -135 to +135 degrees, value below; relative = pointer up ---- */
static const int8_t tip[32][2] = {  /* radius-5 pointer end for 32 steps of the sweep */
    {-4, 4}, {-4, 3}, {-4, 2}, {-5, 2}, {-5, 1}, {-5, 0}, {-5, -1}, {-5, -1}, {-5, -2}, {-4, -3}, {-4, -3},
    {-3, -4}, {-3, -4}, {-2, -5}, {-1, -5}, {0, -5}, {0, -5}, {1, -5}, {2, -5}, {3, -4}, {3, -4}, {4, -3},
    {4, -3}, {5, -2}, {5, -1}, {5, -1}, {5, 0}, {5, 1}, {5, 2}, {4, 2}, {4, 3}, {4, 4}};

static void line(int x0, int y0, int x1, int y1) {  /* Bresenham */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        px(x0, y0);
        if (x0 == x1 && y0 == y1) return;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

static void circle(int cx, int cy, int r) {  /* midpoint circle */
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px(cx + x, cy + y), px(cx - x, cy + y), px(cx + x, cy - y), px(cx - x, cy - y);
        px(cx + y, cy + x), px(cx - y, cy + x), px(cx + y, cy - x), px(cx - y, cy - x);
        y++;
        if (err < 0) err += 2 * y + 1;
        else x--, err += 2 * (y - x) + 1;
    }
}

void draw_knobs(const uint8_t pos[8], const uint8_t value[8], uint8_t rel) {
    fb_clear();
    for (int i = 0; i < 8; i++) {
        int cx = 8 + 16 * i, cy = 9;
        circle(cx, cy, 7);
        int t = rel >> i & 1 ? 15 : pos[i] * 31 / 127;
        line(cx, cy, cx + tip[t][0], cy + tip[t][1]);
        if (!(rel >> i & 1)) {  /* value, right-aligned, no leading zeros */
            int v = value[i];
            for (int d = 2; d >= 0; d--, v /= 10) {
                digit(cx - 5 + 4 * d, 21, v % 10, 1);
                if (v < 10) break;
            }
        }
        digit(cx - 1, 27, i + 1, 1);  /* knob number */
    }
}

/* ---- home, button messages, tempo ---- */
static char *put_int(char *p, int v, int plus) {
    if (v < 0) *p++ = '-', v = -v;
    else if (plus && v > 0) *p++ = '+';
    if (v >= 100) *p++ = '0' + v / 100;
    if (v >= 10) *p++ = '0' + v / 10 % 10;
    *p++ = '0' + v % 10;
    return p;
}

static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

void draw_home(const home_t *h) {
    static const char *const modes[3] = {"NOTES", "CC", "PROG"};
    char l1[24], l2[16], l3[20], *p;
    p = put_s(l1, "OCT");
    p = put_int(p, h->octave, 1);
    p = put_s(p, "  BANK ");
    *p++ = h->bank ? 'B' : 'A';
    p = put_s(p, "  ");
    p = put_s(p, modes[h->pad_mode > 2 ? 0 : h->pad_mode]);
    *p = 0;
    p = h->bpm ? put_int(l2, h->bpm, 0) : put_s(l2, "---");
    p = put_s(p, h->ext ? " BPM EXT" : " BPM");
    *p = 0;
    p = l3;
    if (h->arp) p = put_s(p, "ARP ");
    if (h->repeat) p = put_s(p, "REPEAT ");
    if (h->full) p = put_s(p, "FULL");
    *p = 0;
    fb_clear();
    text(0, 1, l1, 1);
    text(0, 12, l2, 1);
    text(0, 24, l3, 1);
    if (h->usb) rect(122, 26, 5, 5);
}

static int width(const char *s, int scale) { int n = 0; while (s[n]) n++; return n * 6 * scale; }

void draw_msg(const char *title, const char *value) {
    fb_clear();
    text((128 - width(title, 1)) / 2, 1, title, 1);
    text((128 - width(value, 2)) / 2, 14, value, 2);
}

void draw_tempo(int bpm, int ext, int beat) {
    char n[8];
    *(bpm ? put_int(n, bpm, 0) : put_s(n, "---")) = 0;
    fb_clear();
    text(0, 1, ext ? "TEMPO  EXT" : "TEMPO", 1);
    text(0, 11, n, 3);
    text(width(n, 3) + 4, 25, "BPM", 1);
    if (beat) rect(110, 12, 12, 12);
    else frame(110, 12, 12, 12);
}

/* ---- idle image: the uploaded picture or the built-in logo; inverted every minute by the caller ---- */
void draw_idle_image(const uint8_t *img, int invert) {
    if (img) {
        for (int i = 0; i < 512; i++) fb[i] = img[i];
    } else {
        fb_clear();
        text(4, 4, "MPK MINI 3", 2);
        text(19, 22, "CUSTOM FIRMWARE", 1);
    }
    if (invert)
        for (int i = 0; i < 512; i++) fb[i] ^= 0xFF;
}
