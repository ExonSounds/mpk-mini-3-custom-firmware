/* ADC1 (0x40012400), STM32F1-style. Custom fw: single software-started conversions, polls EOC, reads DR.
 * Stock: continuous scan of ch0-4 with DMA (DMA1 ch1, circular) restarted per mux step from TMR3.
 * Conversion time = (sample time + 12.5) ADC clocks, ADC clock = PCLK2 (= F_CPU, APB2 /1) / ADCPRE. */
#include "periph.h"

enum { SR = 0x00, CR1 = 0x04, CR2 = 0x08, SMPR1 = 0x0C, SMPR2 = 0x10, SQR1 = 0x2C, SQR2 = 0x30, SQR3 = 0x34, DR = 0x4C };
static uint32_t r[0x50 / 4];
#define REG(o) r[(o) / 4]
static int idx;  /* rank being converted */

static int seq_len(void) { return REG(CR1) & 0x100 ? (int)(REG(SQR1) >> 20 & 15) + 1 : 1; }  /* SCAN */
static int seq_ch(int k) { return (k < 6 ? REG(SQR3) >> (5 * k) : k < 12 ? REG(SQR2) >> (5 * (k - 6)) : REG(SQR1) >> (5 * (k - 12))) & 31; }

static uint64_t conv_cycles(int ch) {
    static const double smp[8] = {1.5, 7.5, 13.5, 28.5, 41.5, 55.5, 71.5, 239.5};
    static const int div[8] = {2, 4, 6, 8, 2, 12, 8, 16};  /* CFGR ADCPRE bits 15:14 + AT32's bit 28 */
    uint32_t cfgr = bus_read(0x40021004, 4);
    int s = (ch < 10 ? REG(SMPR2) >> (3 * ch) : REG(SMPR1) >> (3 * (ch - 10))) & 7;
    return (uint64_t)((smp[s] + 12.5) * div[(cfgr >> 14 & 3) | (cfgr >> 26 & 4)]);
}

static void irq_update(void) { irq_level(18, (REG(CR1) & 0x20) && (REG(SR) & 2)); }  /* ADC1_2, EOCIE */

static void conv_done(void *ctx);
static void conv_next(void) { ev_at(now() + conv_cycles(seq_ch(idx)), conv_done, 0); }
static void conv_start(void) { idx = 0; REG(SR) |= 0x10; conv_next(); }  /* STRT */

static void conv_done(void *ctx) {
    (void)ctx;
    uint16_t v = analog_in(seq_ch(idx)) & 0xFFF;
    REG(DR) = REG(CR2) & 0x800 ? (uint32_t)v << 4 : v;  /* ALIGN left */
    REG(SR) |= 2;                                       /* EOC */
    irq_update();
    if (REG(CR2) & 0x100) dma_request(1);
    if (++idx < seq_len()) conv_next();
    else if (REG(CR2) & 2) idx = 0, conv_next();        /* CONT */
}

static uint32_t adc_rd(void *ctx, uint32_t off, int size) {
    (void)ctx, (void)size;
    if (off >= sizeof r) return 0;
    uint32_t v = r[off / 4];
    if (off == DR) REG(SR) &= ~2u, irq_update();
    return v;
}

static void adc_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    (void)ctx, (void)size;
    if (off >= sizeof r) return;
    if (off == SR) { REG(SR) &= v; irq_update(); return; }  /* rc_w0 */
    if (off == DR) return;
    if (off != CR2) { r[off / 4] = v; if (off == CR1) irq_update(); return; }
    uint32_t was = REG(CR2);
    REG(CR2) = v & ~0x0060000Cu;  /* calibration finishes at once (CAL, RSTCAL); SWSTART/JSWSTART self-clear */
    if (!(v & 1)) { ev_cancel(conv_done, 0); return; }  /* ADON off: power down */
    /* start: SWSTART, or ADON written 1 again with nothing else changed (F1 rule) */
    if ((was & 1) && ((v & 0x400000) || v == was)) conv_start();
}

static void adc_reset(void *ctx) { (void)ctx; for (unsigned i = 0; i < sizeof r / 4; i++) r[i] = 0; }

void adc1_init(void) {
    mmio_add("ADC1", 0x40012400, 0x400, adc_rd, adc_wr, 0);
    on_reset(adc_reset, 0);
}
