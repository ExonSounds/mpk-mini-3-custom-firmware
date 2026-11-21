#include "board.h"

void delay(volatile uint32_t n) { while (n--) __asm volatile("nop"); }

/* No C library on the target, but the compiler turns struct copies and clears into these calls. */
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, size_t n) {
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)c;
    return d;
}
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }

static void pin_mode(uint32_t port, int pin, uint32_t cfg) {  /* cfg: 0 analog, 1 out push-pull, 4 floating in */
    volatile uint32_t *cr = (volatile uint32_t *)(port + (pin >= 8 ? 4 : 0));
    int s = (pin & 7) * 4;
    *cr = (*cr & ~(0xFu << s)) | (cfg << s);
}

/* Same sequence as stock SystemInit/SetSysClock (0x08013A14/0x08013C5E): HSE 8 MHz x6 = 48 MHz, AHB/1, APB2/1,
 * APB1/2, flash 1 wait state + prefetch, USB clock = PLL/1. If the crystal doesn't start we stay on HSI 8 MHz. */
static void clock_init(void) {
    RCC_APB1ENR |= 1u << 28;
    R32(0x40007050) |= 4;  /* PWC register stock sets before touching clocks */
    RCC_APB1ENR &= ~(1u << 28);
    RCC_CR |= 1;                          /* HSI on */
    RCC_CFGR &= 0xE8FF000C;               /* SYSCLK = HSI, prescalers /1 */
    RCC_CR &= 0xFEF6FFFF;                 /* HSE, CSS, PLL off */
    RCC_CR &= ~(1u << 18);                /* HSE not bypassed */
    RCC_CFGR &= 0x1700FFFF;
    RCC_MISC &= 0xFEFEFF00;
    RCC_CIR = 0x009F0000;                 /* clear clock interrupt flags */

    RCC_CR |= 1u << 16;                   /* HSE on */
    for (int t = 0; t < 0x3000 && !(RCC_CR & (1u << 17)); t++) {}
    if (!(RCC_CR & (1u << 17))) return;
    FLASH_ACR = 0x11;
    RCC_CFGR = (RCC_CFGR & ~0x3F00u) | 0x400;           /* APB2 /1, APB1 /2 */
    RCC_CFGR = (RCC_CFGR & 0x1FC0FFFF) | 0x110000;      /* PLL source HSE, x6 */
    RCC_PLL = (RCC_PLL & ~0x87000000u) | 0x02000000;    /* PLL reference range 7.8-8.33 MHz */
    RCC_CR |= 1u << 24;
    while (!(RCC_CR & (1u << 25))) {}
    RCC_CFGR = (RCC_CFGR & ~3u) | 2;
    while ((RCC_CFGR & 0xC) != 8) {}
    RCC_CFGR = (RCC_CFGR & ~0x08C00000u) | 0x00400000;  /* USB clock = PLL / 1 */
}

/* ---- OLED (SSD1306-type, 128x32), bit-banged SPI mode 0 ---- */
static void oled_byte(uint8_t b) {
    /* ~13 cycles per bit (~270 ns at 48 MHz). With only "nop; nop" the period was ~110 ns, right at the SSD1306's
     * 100 ns minimum, and the fast-redrawing knob view showed occasional glitches. */
    for (int i = 7; i >= 0; i--) {
        if (b >> i & 1) BSRR(GPIOB) = OLED_SDA; else BRR(GPIOB) = OLED_SDA;
        __asm volatile("nop; nop; nop");  /* data setup before the rising edge */
        BSRR(GPIOB) = OLED_SCK;
        __asm volatile("nop; nop; nop");  /* clock high */
        BRR(GPIOB) = OLED_SCK;
        __asm volatile("nop; nop");       /* clock low */
    }
}

static void oled_cmds(const uint8_t *c, int n) {
    BRR(GPIOB) = OLED_DC | OLED_CS;
    while (n--) oled_byte(*c++);
    BSRR(GPIOB) = OLED_CS;
}

static void oled_init(void) {
    /* Same sequence as stock (0x080132A0, charge-pump variant). */
    static const uint8_t init[] = {0xAE, 0x00, 0x10, 0x40, 0x81, 0xCF, 0xA6, 0xA4, 0xA8, 0x3F, 0xD3, 0x00, 0xD5, 0x80,
                                   0xD9, 0xF1, 0xDA, 0x02, 0xDB, 0x20, 0x20, 0x00, 0xA1, 0xC8, 0x8D, 0x14, 0xAF};
    BSRR(GPIOB) = OLED_CS;
    BRR(GPIOB) = OLED_RES;
    delay(100000);
    BSRR(GPIOB) = OLED_RES;
    delay(100000);
    oled_cmds(init, sizeof init);
}

void oled_flush(void) {
    /* The controller is set up for 64 rows but the glass shows only one half of its 8 pages: write both halves. */
    static const uint8_t win[] = {0xD3, 0x00, 0x00, 0x10, 0x40, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
    oled_cmds(win, sizeof win);
    BSRR(GPIOB) = OLED_DC;
    BRR(GPIOB) = OLED_CS;
    for (int i = 0; i < 1024; i++) {
        oled_byte(fb[i & 511]);
        if ((i & 127) == 127) oled_flush_hook();  /* a frame takes a few ms: let MIDI out meanwhile */
    }
    BSRR(GPIOB) = OLED_CS;
}

__attribute__((weak)) void oled_flush_hook(void) {}

void oled_power(int on) {  /* SSD1306 display off/on; RAM is kept, so the next flush shows the screen again */
    uint8_t c = on ? 0xAF : 0xAE;
    oled_cmds(&c, 1);
}

/* ---- LEDs: 3 chained shift registers, byte 0 (LEDs 0-7) first, MSB first, as stock's SPI1 DMA sends it.
 * Outputs are active low (stock clears a LED's bit for its "on" PWM slots, 0x0800DF4E). ---- */
void led_out(uint32_t bits) {
    bits = ~bits;
    BRR(GPIOB) = LED_LAT;
    for (int j = 0; j < 3; j++)
        for (int i = 7; i >= 0; i--) {
            if (bits >> (8 * j + i) & 1) BSRR(GPIOB) = LED_SDA; else BRR(GPIOB) = LED_SDA;
            BSRR(GPIOB) = LED_SCK;
            __asm volatile("nop; nop");
            BRR(GPIOB) = LED_SCK;
        }
    BSRR(GPIOB) = LED_LAT;
}

void adc_init(void) {
    RCC_CFGR |= 3u << 14;              /* ADC clock = APB2 / 8 = 6 MHz */
    R32(0x40012410) = 0x36DB6DB6;      /* SMPR2: 71.5 cycles on ch0-9 (~14 us per conversion) */
    R32(0x4001242C) = 0;               /* SQR1: one conversion per start */
    ADC_CR2 = 1;                       /* ADON */
    delay(10000);
    ADC_CR2 |= 1u << 3;                /* reset calibration */
    for (int t = 0; t < 100000 && (ADC_CR2 & (1u << 3)); t++) {}
    ADC_CR2 |= 1u << 2;                /* calibrate */
    for (int t = 0; t < 100000 && (ADC_CR2 & (1u << 2)); t++) {}
    ADC_CR2 |= (7u << 17) | (1u << 20);  /* trigger = software start */
}

void matrix_drive(int row) {
    ODR(GPIOC) = row < 8 ? 0xFF & ~(1u << row) : 0xFF;
    if (row == 8) BSRR(GPIOB) = (1u << 9) | (1u << (8 + 16));
    else if (row == 9) BSRR(GPIOB) = (1u << 8) | (1u << (9 + 16));
    else BSRR(GPIOB) = (1u << 8) | (1u << 9);
}

void board_init(void) {
    clock_init();
    R32(0xE000EDFC) |= 1u << 24;  /* DEMCR.TRCENA */
    R32(0xE0001000) |= 1;         /* DWT cycle counter on */
    RCC_APB2ENR |= (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 9);  /* IOMUX, GPIOA-C, ADC1 */
    AFIO_MAPR = (AFIO_MAPR & ~(7u << 24)) | (2u << 24);  /* JTAG off, SWD on: frees PB3/PB4 for the LEDs */

    for (int p = 0; p < 5; p++) pin_mode(GPIOA, p, 0);   /* PA0-4 analog */
    pin_mode(GPIOA, 10, 1);
    BSRR(GPIOA) = 1u << 10;                               /* stock drives PA10 high at init */
    for (int p = 0; p < 8; p++) pin_mode(GPIOC, p, 1);   /* matrix drive */
    for (int p = 8; p < 16; p++) pin_mode(GPIOC, p, 4);  /* matrix sense (external pull-ups) */
    static const uint8_t outb[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 14, 15};
    for (unsigned i = 0; i < sizeof outb; i++) pin_mode(GPIOB, outb[i], 1);
    BSRR(GPIOB) = LED_EN | (1u << 8) | (1u << 9);
    ODR(GPIOC) = 0xFF;

    led_out(0);
    oled_init();
    R32(0xE000E180) = R32(0xE000E184) = R32(0xE000E188) = 0xFFFFFFFF;  /* disable IRQs the bootloader left on */
    R32(0xE000E280) = R32(0xE000E284) = R32(0xE000E288) = 0xFFFFFFFF;  /* and clear pending ones */
}

/* ---- Startup and vector table ---- */
extern uint32_t _estack[], _sidata[], _sdata[], _edata[], _sbss[], _ebss[];
int main(void);

__attribute__((naked, noreturn)) void Reset_Handler(void) {
    /* The bootloader jumps here without loading our stack pointer. */
    __asm volatile("ldr sp, =_estack\n b start");
}

__attribute__((noreturn, used)) void start(void) {
    R32(0xE000ED08) = 0x0800A000;  /* VTOR */
    for (uint32_t *s = _sidata, *d = _sdata; d < _edata;) *d++ = *s++;
    for (uint32_t *d = _sbss; d < _ebss;) *d++ = 0;
    main();
    for (;;) {}
}

void Default_Handler(void) { for (;;) {} }
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void OTGFS_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".vectors"), used)) void (*const vectors[16 + 76])(void) = {
    [0] = (void (*)(void))_estack, [1] = Reset_Handler,
    [2 ... 6] = Default_Handler, [11 ... 12] = Default_Handler, [14] = Default_Handler,
    [15] = SysTick_Handler,
    [16 ... 16 + 66] = Default_Handler,
    [16 + 67] = OTGFS_IRQHandler,  /* same IRQ number as stock */
    [16 + 68 ... 16 + 75] = Default_Handler,
};
