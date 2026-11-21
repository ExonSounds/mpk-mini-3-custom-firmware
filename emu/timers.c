/* TMR1-TMR5 time base (STM32F1-style registers): up-counting CNT, PSC, ARR, update flag (SR.UIF) and update
 * interrupt (DIER.UIE) on TMR1_UP 25, TMR2 28, TMR3 29, TMR4 30, TMR5 50. Stock only takes TMR3's interrupt
 * (vector 0x08011CE5: analog step + OLED state machine); the others only need to count.
 * shortcut: timer clock = 48 MHz for all (APB2 /1; APB1 /2 doubles to 48 MHz), no down/centre counting, no
 * capture/compare or outputs (no timer pins are used), PSC/ARR changes act at once. The counter is computed
 * from the cycle count, so a timer costs an event only while its update interrupt is enabled. */
#include "periph.h"

typedef struct {
    const char *name;
    uint32_t base;
    int irq, bits32;
    uint32_t r[0x50 / 4];
    uint64_t t0;   /* cycle at which CNT was 0 (while running) */
    uint32_t cnt;  /* CNT while stopped */
} tmr_t;
static tmr_t T[5] = {{"TMR1", 0x40012C00, 25, 0, {0}, 0, 0}, {"TMR2", 0x40000000, 28, 1, {0}, 0, 0},
                     {"TMR3", 0x40000400, 29, 0, {0}, 0, 0}, {"TMR4", 0x40000800, 30, 0, {0}, 0, 0},
                     {"TMR5", 0x40000C00, 50, 1, {0}, 0, 0}};
enum { CR1 = 0x00, DIER = 0x0C, SR = 0x10, EGR = 0x14, CNT = 0x24, PSC = 0x28, ARR = 0x2C };
#define REG(t, o) (t)->r[(o) / 4]

static uint64_t tick(tmr_t *t) { return (uint64_t)(REG(t, PSC) & 0xFFFF) + 1; }  /* cycles per count */
static uint64_t period(tmr_t *t) { return ((uint64_t)REG(t, ARR) + 1) * tick(t); }
static int running(tmr_t *t) { return REG(t, CR1) & 1; }

/* bring SR.UIF and CNT up to now: an update happens every period() cycles after t0 */
static void tsync(tmr_t *t) {
    if (!running(t) || !REG(t, ARR)) return;
    uint64_t n = now(), p = period(t);
    if (n - t->t0 < p) return;
    REG(t, SR) |= 1;
    if (REG(t, CR1) & 8) { REG(t, CR1) &= ~1u, t->cnt = 0; return; }  /* one-pulse mode stops at the update */
    t->t0 += (n - t->t0) / p * p;
}
static uint32_t cnt(tmr_t *t) { return running(t) ? (uint32_t)((now() - t->t0) / tick(t)) : t->cnt; }

static void fire(void *ctx);
static void update(tmr_t *t) {  /* interrupt line and the next event */
    irq_level(t->irq, (REG(t, SR) & REG(t, DIER) & 1) != 0);
    if (running(t) && (REG(t, DIER) & 1) && REG(t, ARR)) ev_at(t->t0 + period(t), fire, t);
    else ev_cancel(fire, t);
}
static void fire(void *ctx) { tsync(ctx); update(ctx); }

static uint32_t tmr_rd(void *ctx, uint32_t off, int size) {
    tmr_t *t = ctx;
    (void)size;
    if (off >= sizeof t->r) return 0;
    tsync(t);
    if (off == CNT) return cnt(t);
    return t->r[off / 4];
}

static void tmr_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    tmr_t *t = ctx;
    (void)size;
    if (off >= sizeof t->r) return;
    tsync(t);
    uint32_t c = cnt(t);
    if (!t->bits32 && (off == ARR || off == CNT)) v &= 0xFFFF;
    switch (off) {
    case SR: REG(t, SR) &= v; break;  /* rc_w0 */
    case EGR:
        if (v & 1) {  /* UG: counter restarts; UIF unless URS */
            c = 0;
            if (!(REG(t, CR1) & 4)) REG(t, SR) |= 1;
        }
        break;
    case CNT: c = v; break;
    default: t->r[off / 4] = v;
    }
    if (running(t)) t->t0 = now() - (uint64_t)c * tick(t);  /* keep CNT across PSC/ARR/CR1 writes */
    else t->cnt = c;
    update(t);
}

static void tmr_reset(void *ctx) {
    tmr_t *t = ctx;
    for (unsigned i = 0; i < sizeof t->r / 4; i++) t->r[i] = 0;
    REG(t, ARR) = t->bits32 ? 0xFFFFFFFF : 0xFFFF;
    t->cnt = 0, t->t0 = 0;
}

void tmr_init(void) {
    for (int i = 0; i < 5; i++) {
        mmio_add(T[i].name, T[i].base, 0x400, tmr_rd, tmr_wr, &T[i]);
        on_reset(tmr_reset, &T[i]);
    }
}
