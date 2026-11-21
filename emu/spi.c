/* SPI1 (0x40013000, APB2 = 48 MHz; stock: LEDs on remapped PB3/PB5) and SPI2 (0x40003800, APB1 = 24 MHz; stock:
 * OLED on PB13/PB15), master transmit only, as both firmwares use them: a byte written to DR (by the CPU or by
 * DMA when CR2.TXDMAEN is set) shifts out in 8 SPI clocks and then goes to the sink. SR: TXE, BSY, RXNE. */
#include "periph.h"

typedef struct {
    const char *name;
    uint32_t base, pclk;
    int dma_ch;
    void (*sink)(uint8_t);
    uint32_t cr1, cr2, other[8];
    int busy, full, rxne;
    uint8_t tx, shift;
} spi_t;
static spi_t S[2] = {{"SPI1", 0x40013000, 48000000, 3, leds_spi_byte, 0, 0, {0}, 0, 0, 0, 0, 0},
                     {"SPI2", 0x40003800, 24000000, 5, oled_spi_byte, 0, 0, {0}, 0, 0, 0, 0, 0}};

static void shifted(void *ctx);
static void start_byte(spi_t *s, uint8_t b) {
    s->shift = b, s->busy = 1;
    uint64_t t = (uint64_t)8 * (2u << (s->cr1 >> 3 & 7)) * (F_CPU / s->pclk);  /* BR: PCLK / 2^(BR+1) */
    ev_at(now() + t, shifted, s);
}
static void feed(spi_t *s) {  /* let DMA fill the shift register and the TX buffer */
    while ((s->cr2 & 2) && (s->cr1 & 0x40) && !s->full && dma_request(s->dma_ch)) {}
}
static void put(spi_t *s, uint8_t b) {
    if (!(s->cr1 & 0x40)) return;  /* SPE off */
    if (!s->busy) start_byte(s, b);
    else s->tx = b, s->full = 1;
}
static void shifted(void *ctx) {
    spi_t *s = ctx;
    s->sink(s->shift);
    if (!((s->cr1 & 0xC000) == 0xC000)) s->rxne = 1;  /* bidirectional transmit-only receives nothing */
    s->busy = 0;
    if (s->full) s->full = 0, start_byte(s, s->tx);
    feed(s);
}

static uint32_t spi_rd(void *ctx, uint32_t off, int size) {
    spi_t *s = ctx;
    (void)size;
    switch (off) {
    case 0x00: return s->cr1;
    case 0x04: return s->cr2;
    case 0x08: return (uint32_t)(!s->full) << 1 | (uint32_t)(s->busy || s->full) << 7 | (uint32_t)s->rxne;
    case 0x0C: s->rxne = 0; return 0xFF;
    }
    return off < 0x30 ? s->other[(off - 0x10) / 4 % 8] : 0;
}
static void spi_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    spi_t *s = ctx;
    (void)size;
    switch (off) {
    case 0x00: s->cr1 = v & 0xFFFF; feed(s); return;
    case 0x04: s->cr2 = v & 0xFFFF; feed(s); return;
    case 0x08: return;
    case 0x0C: put(s, (uint8_t)v); return;
    }
    if (off < 0x30) s->other[(off - 0x10) / 4 % 8] = v;
}
static void spi_kick(void *ctx) { feed(ctx); }
static void spi_reset(void *ctx) {
    spi_t *s = ctx;
    s->cr1 = s->cr2 = 0, s->busy = s->full = s->rxne = 0;
}

void spi_init(void) {
    for (int i = 0; i < 2; i++) {
        mmio_add(S[i].name, S[i].base, 0x400, spi_rd, spi_wr, &S[i]);
        on_reset(spi_reset, &S[i]);
        dma_on_enable(S[i].dma_ch, spi_kick, &S[i]);
    }
}
