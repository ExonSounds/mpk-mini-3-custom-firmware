/* GPIO ports A-D (STM32F1-style CRL/CRH/IDR/ODR/BSRR/BRR/LCKR; the AT32F415 keeps the layout). Models of the
 * things wired to pins subscribe with gpio_watch() (outputs) and gpio_input() (inputs), see core.h. */
#include "core.h"
#include <string.h>

#define NPORT 4
#define NWATCH 8
static struct {
    uint32_t cr[2], odr, lck;
    uint16_t levels;
    gpio_input_fn in;
    void *in_ctx;
    struct { gpio_watch_fn fn; void *ctx; } w[NWATCH];
    int nw;
} P[NPORT];

static uint16_t out_mask(int p) {  /* pins in an output mode (MODE != 0) */
    uint16_t m = 0;
    for (int i = 0; i < 16; i++) if (P[p].cr[i / 8] >> (4 * (i % 8)) & 3) m |= 1u << i;
    return m;
}
uint16_t gpio_odr(int p) { return (uint16_t)P[p].odr; }
uint16_t gpio_out(int p) { uint16_t m = out_mask(p); return (uint16_t)((P[p].odr & m) | ~m); }

void gpio_watch(int p, gpio_watch_fn fn, void *ctx) { P[p].w[P[p].nw].fn = fn, P[p].w[P[p].nw].ctx = ctx, P[p].nw++; }
void gpio_input(int p, gpio_input_fn fn, void *ctx) { P[p].in = fn, P[p].in_ctx = ctx; }

static void changed(int p) {
    uint16_t old = P[p].levels, nw = gpio_out(p);
    if (old == nw) return;
    P[p].levels = nw;
    for (int i = 0; i < P[p].nw; i++) P[p].w[i].fn(P[p].w[i].ctx, p, old, nw);
}

static uint32_t gpio_rd(void *ctx, uint32_t off, int size) {
    int p = (int)(intptr_t)ctx;
    (void)size;
    switch (off & ~3u) {
    case 0x00: return P[p].cr[0];
    case 0x04: return P[p].cr[1];
    case 0x08: {
        uint16_t m = out_mask(p), pull = 0xFFFF;
        for (int i = 0; i < 16; i++)  /* input with pull-down (CNF=10, ODR=0) reads 0 when nothing drives it */
            if ((P[p].cr[i / 8] >> (4 * (i % 8)) & 0xF) == 8 && !(P[p].odr >> i & 1)) pull &= ~(1u << i);
        uint16_t in = P[p].in ? P[p].in(P[p].in_ctx, p) & pull : pull;
        return (P[p].odr & m) | (in & ~m);
    }
    case 0x0C: return P[p].odr;
    case 0x18: return P[p].lck;
    }
    return 0;
}

static void gpio_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    int p = (int)(intptr_t)ctx;
    if (size != 4) {  /* byte/halfword stores: merge (BSRR high half by halfword store is common) */
        uint32_t sh = 8 * (off & 3), m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
        uint32_t cur = (off & ~3u) == 0x10 || (off & ~3u) == 0x14 ? 0 : gpio_rd(ctx, off & ~3u, 4);
        v = (cur & ~m) | (v << sh & m);
        off &= ~3u;
    }
    switch (off) {
    case 0x00: P[p].cr[0] = v; break;
    case 0x04: P[p].cr[1] = v; break;
    case 0x0C: P[p].odr = v & 0xFFFF; break;
    case 0x10: P[p].odr = (P[p].odr | (v & 0xFFFF)) & ~(v >> 16); break;
    case 0x14: P[p].odr &= ~(v & 0xFFFF); break;
    case 0x18: P[p].lck = v; break;
    default: return;
    }
    changed(p);
}

static void gpio_reset(void *ctx) {
    int p = (int)(intptr_t)ctx;
    P[p].cr[0] = P[p].cr[1] = 0x44444444;  /* floating inputs */
    P[p].odr = P[p].lck = 0;
    P[p].levels = 0xFFFF;
}

void gpio_init(void) {
    static const char *names[NPORT] = {"GPIOA", "GPIOB", "GPIOC", "GPIOD"};
    for (int p = 0; p < NPORT; p++) {
        mmio_add(names[p], 0x40010800 + 0x400 * p, 0x400, gpio_rd, gpio_wr, (void *)(intptr_t)p);
        on_reset(gpio_reset, (void *)(intptr_t)p);
    }
}
