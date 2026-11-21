/* MPK mini 3 (AT32F415RBT7) board support: registers, pins, clocks, OLED, LEDs. Pin map: firmware/NOTES.md.
 * Peripheral registers are STM32F1-compatible on this part (the stock firmware uses ST's StdPeriph library). */
#pragma once
#include <stdint.h>

#define R32(a) (*(volatile uint32_t *)(a))
#define RCC_CR      R32(0x40021000)
#define RCC_CFGR    R32(0x40021004)
#define RCC_CIR     R32(0x40021008)
#define RCC_AHBENR  R32(0x40021014)
#define RCC_APB2ENR R32(0x40021018)
#define RCC_APB1ENR R32(0x4002101C)
#define RCC_PLL     R32(0x4002102C)  /* AT32-specific */
#define RCC_MISC    R32(0x40021030)  /* AT32-specific */
#define FLASH_ACR   R32(0x40022000)
#define AFIO_MAPR   R32(0x40010004)
#define GPIOA 0x40010800u
#define GPIOB 0x40010C00u
#define GPIOC 0x40011000u
#define IDR(p)  R32((p) + 0x08)
#define ODR(p)  R32((p) + 0x0C)
#define BSRR(p) R32((p) + 0x10)
#define BRR(p)  R32((p) + 0x14)
#define SYST_CSR R32(0xE000E010)
#define SYST_RVR R32(0xE000E014)
#define SYST_CVR R32(0xE000E018)
#define NVIC_ISER(n) R32(0xE000E100 + 4 * (n))
#define DWT_CYCCNT R32(0xE0001004)

#define F_CPU 48000000u

/* Pins (GPIOB unless noted) */
#define OLED_RES (1u << 10)
#define OLED_CS  (1u << 12)
#define OLED_SCK (1u << 13)
#define OLED_DC  (1u << 14)
#define OLED_SDA (1u << 15)
#define LED_SCK  (1u << 3)
#define LED_LAT  (1u << 4)
#define LED_SDA  (1u << 5)
#define LED_EN   (1u << 7)

#ifdef __arm__
static inline void irq_enable(void) { __asm volatile("cpsie i" ::: "memory"); }
static inline void irq_disable(void) { __asm volatile("cpsid i" ::: "memory"); }
#else
static inline void irq_enable(void) {}  /* host builds (make check) */
static inline void irq_disable(void) {}
#endif

void delay(volatile uint32_t n);
void board_init(void);          /* clocks (48 MHz from the 8 MHz crystal, like stock), pins, OLED, LEDs off */

extern uint8_t fb[512];         /* 128x32, fb[page * 128 + x], bit = y % 8 */
void oled_flush(void);
void oled_power(int on);
void oled_flush_hook(void);   /* called during oled_flush (weak, default empty); main.c sends queued MIDI */
void fb_clear(void);
void px(int x, int y);
void rect(int x, int y, int w, int h);
void digit(int x, int y, int d, int scale);  /* 3x5 font, each dot drawn scale x scale */
int text(int x, int y, const char *s, int scale);  /* 5x7 font, 6*scale px per char; returns x after the text */

void system_reset(void);      /* restart the chip (bootloader, then this firmware) */
void led_out(uint32_t bits);    /* bit n = LED n (0-23), see firmware/NOTES.md */

/* ADC1 on PA0-PA4 (ch 0-4). PA0-PA2 go through 8:1 muxes selected by PB0-PB2 (firmware/NOTES.md). */
#define ADC_SR  R32(0x40012400)
#define ADC_CR2 R32(0x40012408)
#define ADC_SQR3 R32(0x40012434)
#define ADC_DR  R32(0x4001244C)
void adc_init(void);
static inline void adc_start(int ch) { ADC_SQR3 = ch; ADC_CR2 |= 1u << 22; }
static inline int adc_done(void) { return ADC_SR & 2; }
static inline uint16_t adc_value(void) { return ADC_DR & 0xFFF; }
static inline void mux_select(int m) { BSRR(GPIOB) = (m & 7) | ((~m & 7) << 16); }

/* Key/button matrix: drive row r (0-7 = PC0-7, 8 = PB8, 9 = PB9) low; read with matrix_read(). */
void matrix_drive(int row);
static inline uint8_t matrix_read(void) { return ~(IDR(GPIOC) >> 8) & 0xFF; }  /* bit set = switch closed */
