/* DMA1 (0x40020000), STM32F1-style: ISR/IFCR, 7 channels of CCR/CNDTR/CPAR/CMAR. Requests come from the
 * peripheral models (dma_request: ADC1 -> ch1, SPI1 TX -> ch3, SPI2 TX -> ch5); every request moves one item
 * at once (shortcut: no bus arbitration or transfer time, the requesting peripheral's pace is the timing). */
#include "periph.h"

static struct { uint32_t ccr, cndtr, cpar, cmar, n0, pa, ma; void (*kick)(void *); void *kctx; } C[8];
static uint32_t isr;

static void irq_update(int ch) {  /* DMA1 channel n = IRQ 10 + n; TCIE/HTIE/TEIE are CCR bits 1-3 */
    irq_level(10 + ch, (isr >> (4 * (ch - 1)) & C[ch].ccr & 0xE) != 0);
}

int dma_request(int ch) {
    typeof(C[0]) *c = &C[ch];
    if (!(c->ccr & 1) || !c->cndtr) return 0;
    int psz = 1 << (c->ccr >> 8 & 3), msz = 1 << (c->ccr >> 10 & 3);
    if (c->ccr & 0x10) bus_write(c->pa, bus_read(c->ma, msz), psz);  /* DIR: memory -> peripheral */
    else bus_write(c->ma, bus_read(c->pa, psz), msz);
    if (c->ccr & 0x40) c->pa += psz;
    if (c->ccr & 0x80) c->ma += msz;
    int sh = 4 * (ch - 1);
    if (--c->cndtr == c->n0 / 2) isr |= 5u << sh;  /* HTIF + GIF */
    if (!c->cndtr) {
        isr |= 3u << sh;                          /* TCIF + GIF */
        if (c->ccr & 0x20) c->cndtr = c->n0, c->pa = c->cpar, c->ma = c->cmar;  /* circular */
    }
    irq_update(ch);
    return 1;
}

void dma_on_enable(int ch, void (*fn)(void *), void *ctx) { C[ch].kick = fn, C[ch].kctx = ctx; }

static uint32_t dma_rd(void *ctx, uint32_t off, int size) {
    (void)ctx, (void)size;
    if (off == 0) return isr;
    if (off == 4) return 0;
    int ch = (off - 8) / 20 + 1, r = (off - 8) % 20;
    if (off < 8 || ch > 7) { mmio_unmodelled("DMA1", 0x40020000 + off, 0, 0); return 0; }
    return r == 0 ? C[ch].ccr : r == 4 ? C[ch].cndtr : r == 8 ? C[ch].cpar : r == 12 ? C[ch].cmar : 0;
}

static void dma_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    (void)ctx, (void)size;
    if (off == 4) {  /* IFCR: CGIFx clears all four flags of channel x */
        for (int ch = 1; ch <= 7; ch++) {
            uint32_t m = v >> (4 * (ch - 1)) & 0xF;
            if (m & 1) m = 0xF;
            isr &= ~(m << (4 * (ch - 1)));
            irq_update(ch);
        }
        return;
    }
    int ch = (off - 8) / 20 + 1, r = (off - 8) % 20;
    if (off < 8 || ch > 7) { mmio_unmodelled("DMA1", 0x40020000 + off, 1, v); return; }
    typeof(C[0]) *c = &C[ch];
    if (r == 0) {
        uint32_t was = c->ccr;
        c->ccr = v & 0x7FFF;
        if ((v & 1) && !(was & 1)) {
            c->pa = c->cpar, c->ma = c->cmar;
            if (v & 0x4000) while (dma_request(ch)) {}  /* memory to memory: runs by itself */
            else if (c->kick) c->kick(c->kctx);
        }
        irq_update(ch);
    } else if (r == 4) {
        if (!(c->ccr & 1)) c->cndtr = c->n0 = v & 0xFFFF;  /* writable only while the channel is off */
    } else if (r == 8) c->cpar = v;
    else if (r == 12) c->cmar = v;
}

static void dma_reset(void *ctx) {
    (void)ctx;
    for (int ch = 1; ch <= 7; ch++) C[ch].ccr = C[ch].cndtr = C[ch].cpar = C[ch].cmar = C[ch].n0 = 0;
    isr = 0;
}

void dma_init(void) {
    mmio_add("DMA1", 0x40020000, 0x400, dma_rd, dma_wr, 0);
    on_reset(dma_reset, 0);
}
