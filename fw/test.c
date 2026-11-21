/* Hardware test firmware: shows the raw key/button matrix and all 26 analog readings on the OLED and lights one
 * LED at a time (OCT+ / OCT- step through LED 0-23). No USB. Back to update mode: hold PAD BANK + PROG SELECT
 * while plugging in. */
#include "board.h"

static volatile uint8_t led_on = 0;

void SysTick_Handler(void) {  /* LEDs at 1/8 duty, like stock's brightness cap of 4/32 */
    static uint8_t phase;
    phase = (phase + 1) & 7;
    if (phase == 0) led_out(1u << led_on);
    else if (phase == 1) led_out(0);
}

static uint8_t mat[10];
static uint16_t ana[26];  /* 0-23: PA0/PA1/PA2 x mux 0-7, 24: PA3, 25: PA4 */

static uint16_t adc_read(int ch) {
    adc_start(ch);
    for (int t = 0; t < 100000; t++)  /* bounded: a register mismatch shows as empty bars, not a hang */
        if (adc_done()) return adc_value();
    return 0;
}

int main(void) {
    board_init();
    adc_init();
    SYST_RVR = 20000;
    SYST_CVR = 0;
    SYST_CSR = 7;
    irq_enable();

    uint8_t prev8 = 0;
    for (uint32_t frame = 0;; frame++) {
        for (int r = 0; r < 10; r++) {
            matrix_drive(r);
            delay(100);
            mat[r] = matrix_read();
        }
        for (int m = 0; m < 8; m++) {
            mux_select(m);
            delay(300);
            for (int c = 0; c < 3; c++) ana[c * 8 + m] = adc_read(c);
        }
        ana[24] = adc_read(3);
        ana[25] = adc_read(4);

        uint8_t edge = mat[8] & ~prev8;
        prev8 = mat[8];
        if (edge & (1u << 6)) led_on = (led_on + 1) % 24;   /* OCT+ */
        if (edge & (1u << 7)) led_on = (led_on + 23) % 24;  /* OCT- */

        fb_clear();
        for (int r = 0; r < 10; r++)
            for (int b = 0; b < 8; b++) {
                if (mat[r] >> b & 1) rect(r * 3, b * 4, 2, 3);
                else px(r * 3, b * 4 + 1);
            }
        digit(31, 0, led_on / 10, 1);
        digit(35, 0, led_on % 10, 1);
        for (int k = 0; k < 26; k++) {
            int x = 41 + 3 * k + 2 * (k / 8), h = 1 + ana[k] * 30 / 4095;
            rect(x, 32 - h, 2, h);
        }
        if (frame & 1) rect(124, 0, 4, 4);  /* heartbeat */
        oled_flush();
    }
}
