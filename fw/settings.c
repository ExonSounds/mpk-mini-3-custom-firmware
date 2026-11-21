#include "settings.h"
#include "board.h"

settings_t S;

/* Defaults = stock program 1 ("Live"). */
void settings_defaults(void) {
    S = (settings_t){.keys_ch = 0, .pads_ch = 9, .transpose = 12, .key_curve = CURVE_NORMAL, .pad_curve = CURVE_NORMAL,
                     .pad_note = 36, .knob_cc = {1, 2, 3, 4, 5, 6, 7, 8}, .knob_speed = 1, .joy_x_cc = 0, .joy_y_cc = 1,
                     .arp_mode = 0, .arp_div = 4, .arp_oct = 0, .arp_latch = 0, .tempo = 130, .screen_off = 1, .clock_ext = 0};
}

int curve(int v, int which) {
    switch (which) {
    case CURVE_SOFT: return 127 - (127 - v) * (127 - v) / 127;
    case CURVE_HARD: { int h = v * v / 127; return h < 1 ? 1 : h; }
    case CURVE_FIXED: return 127;
    default: return v;
    }
}

/* Record: magic, settings bytes, sum16 of the settings bytes. */
#define MAGIC 0x334B504Du  /* "MPK3" (v6 layout) */

static uint16_t sum(const uint8_t *p, int n) {
    uint16_t s = 0;
    while (n--) s += *p++;
    return s;
}

static int valid(const settings_t *s) {
    const uint8_t *c = (const uint8_t *)s;
    static const uint8_t lim[] = {15, 15, 24, 3, 3, 112, 127, 127, 127, 127, 127, 127, 127, 127, 2, 127, 127,
                                  5, 7, 3, 1, 240, 4, 1};
    _Static_assert(sizeof lim == sizeof(settings_t), "one limit per settings byte");
    for (unsigned i = 0; i < sizeof *s; i++) if (c[i] > lim[i]) return 0;
    return s->tempo >= 40;
}

void settings_load(void) {
    const uint8_t *f = (const uint8_t *)SETTINGS_ADDR;
    settings_t s;
    uint8_t *d = (uint8_t *)&s;
    for (unsigned i = 0; i < sizeof s; i++) d[i] = f[4 + i];
    uint16_t stored = f[4 + sizeof s] | f[5 + sizeof s] << 8;
    if (*(const uint32_t *)f == MAGIC && stored == sum(d, sizeof s) && valid(&s)) S = s;
    else settings_defaults();
}

/* STM32F1-style flash controller (stock uses the same registers: 0x08013E.. / bootloader 0x080033E4). */
#define FLASH_KEYR R32(0x40022004)
#define FLASH_SR   R32(0x4002200C)
#define FLASH_CR   R32(0x40022010)
#define FLASH_AR   R32(0x40022014)

static void wait_flash(void) { while (FLASH_SR & 1) {} }

static void program16(uint32_t addr, uint16_t v) {
    FLASH_CR |= 1;  /* PG */
    *(volatile uint16_t *)addr = v;
    wait_flash();
    FLASH_CR &= ~1u;
}

int settings_save(void) {
    uint8_t rec[4 + sizeof S + 2];
    *(uint32_t *)rec = MAGIC;
    const uint8_t *src = (const uint8_t *)&S;
    for (unsigned i = 0; i < sizeof S; i++) rec[4 + i] = src[i];
    uint16_t s = sum(src, sizeof S);
    rec[4 + sizeof S] = s & 0xFF, rec[5 + sizeof S] = s >> 8;

    irq_disable();  /* the CPU stalls on flash during erase anyway; keep ISRs off the bus */
    FLASH_KEYR = 0x45670123;
    FLASH_KEYR = 0xCDEF89AB;
    wait_flash();
    FLASH_SR = 0x34;                     /* clear EOP, WRPRTERR, PRGERR */
    FLASH_CR |= 2;                       /* PER: erase the sector holding SETTINGS_ADDR (1 or 2 KB) */
    FLASH_AR = SETTINGS_ADDR;
    FLASH_CR |= 1u << 6;                 /* STRT */
    wait_flash();
    FLASH_CR &= ~2u;
    for (unsigned i = 0; i < sizeof rec; i += 2) program16(SETTINGS_ADDR + i, rec[i] | rec[i + 1] << 8);
    FLASH_CR |= 1u << 7;                 /* LOCK */
    irq_enable();

    const uint8_t *f = (const uint8_t *)SETTINGS_ADDR;
    for (unsigned i = 0; i < sizeof rec; i++) if (f[i] != rec[i]) return 0;
    return 1;
}
