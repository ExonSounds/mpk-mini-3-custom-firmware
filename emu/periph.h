/* Links between the phase-2 peripheral models (inputs.c, adc.c, dma.c, spi.c, timers.c, oled.c, leds.c). */
#pragma once
#include "core.h"

/* dma.c: DMA1 channel 1-7 */
int dma_request(int ch);                                   /* one transfer if the channel is on; 1 = done */
void dma_on_enable(int ch, void (*fn)(void *ctx), void *ctx);  /* told when the channel gets enabled */

/* inputs.c */
uint16_t analog_in(int ch);   /* what ADC1 channel ch reads now (0-4095), mux select taken from PB0-2 */

/* byte sinks of the SPI ports (spi.c calls them when a byte has been shifted out) */
void oled_spi_byte(uint8_t b);   /* SPI2 -> SSD1306 (oled.c) */
void leds_spi_byte(uint8_t b);   /* SPI1 -> LED shift registers (leds.c) */

void inputs_init(void), adc1_init(void), dma_init(void), spi_init(void), tmr_init(void), oled_init(void), leds_init(void);
