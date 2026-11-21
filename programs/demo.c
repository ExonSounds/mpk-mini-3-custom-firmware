/* Demo RAM program: every key plays with a fifth on top, pads light up by pressure, the joystick moves a ball,
 * the knobs show as bars. PROG SELECT goes back to the normal firmware.
 *   make -C programs && python3 mpk3_cfg.py run programs/demo.bin   (or load "Fifths" in mpk3_market.py) */
#include "prog_api.h"

static void note(const prog_api_t *api, int n, int vel) {
    if (n > 127) return;
    api->midi_send(vel ? 0x09 | 0x90u << 8 | (uint32_t)n << 16 | (uint32_t)vel << 24
                       : 0x08 | 0x80u << 8 | (uint32_t)n << 16 | 0x40u << 24);
}

static volatile uint8_t quit;

static unsigned message(const uint8_t *in, unsigned n, uint8_t *out) {  /* the game market's QUIT (op 2) */
    if (n && in[0] == 2) quit = 1;
    out[0] = 0;
    return 1;
}

int program(const prog_api_t *api) {
    uint8_t vel[25], was[25] = {0}, pads[8], pos[8], val[8], rel;
    uint32_t last = *api->ticks;
    *api->own_input = 1;  /* we play the keys; the firmware stays quiet */
    if (api->version >= 2) *api->on_message = message;
    while (!quit && !(*api->buttons & 1u << MPK_BTN_PROG_SELECT)) {
        api->poll();
        api->keys(vel);
        for (int k = 0; k < 25; k++)
            if (!vel[k] != !was[k]) note(api, 48 + k, vel[k]), note(api, 55 + k, vel[k] * 3 / 4), was[k] = vel[k];
        api->pads(pads);
        for (int i = 0; i < 8; i++) api->led_level[MPK_LED_PAD0 + i] = pads[i] / 4;

        if (*api->ticks - last < 660) continue;  /* draw ~30 times a second */
        last = *api->ticks;
        int x, y;
        api->joystick(&x, &y);
        api->knobs(pos, val, &rel);
        api->fb_clear();
        api->text(0, 0, "RAM PROGRAM", 1);
        for (int i = 0; i < 8; i++) api->rect(72 + 7 * i, 31 - pos[i] / 6, 5, pos[i] / 6 + 1);
        api->rect(30 + x * 28 / 1000, 19 - y * 9 / 1000, 4, 4);
        for (int k = 0; k < 25; k++) if (vel[k]) api->rect(2 * k, 9, 2, 2);
        api->flush();
    }
    for (int k = 0; k < 25; k++) if (was[k]) note(api, 48 + k, 0), note(api, 55 + k, 0);
    api->poll();
    return 0;
}
