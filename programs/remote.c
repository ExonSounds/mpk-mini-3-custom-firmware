/* Remote screen and controller: a program on the computer draws the screen and reads the controls
 * (mpk3_remote.py; e.g. python3 mpk3_race.py). PROG SELECT goes back to the normal firmware.
 * Host messages (SysEx 0B), first byte = op:
 *   1 FRAME  chunk 0-3, then 128 bytes of the frame (fb layout) 7-bit packed (147 bytes); chunk 3 shows it
 *   2 QUIT   back to the normal firmware
 * Every message is answered with the controls: pads 1-8 (0-127), joystick x, y (+1000, 2 x 7 bits each),
 * buttons (2 x 7 bits, bit = MPK_BTN_*), 25 key velocities, 8 knob positions = 47 bytes. */
#include "prog_api.h"

static const prog_api_t *api;
static volatile uint8_t show, quit, connected;
static uint32_t last_msg;

static void unpack7(const uint8_t *in, uint8_t *out, unsigned n) {  /* groups: MSB byte, then up to 7 bytes */
    for (unsigned i = 0; i < n; i += 7, in += 8)
        for (unsigned j = 0; j < 7 && i + j < n; j++) out[i + j] = in[1 + j] | (in[0] >> j & 1) << 7;
}

static unsigned message(const uint8_t *in, unsigned n, uint8_t *out) {
    if (n == 149 && in[0] == 1 && in[1] < 4) {
        unpack7(in + 2, api->fb + 128 * in[1], 128);
        if (in[1] == 3) show = 1;
    } else if (n >= 1 && in[0] == 2) {
        quit = 1;
    }
    connected = 1, last_msg = *api->ticks;
    int x, y;
    uint8_t rel;
    api->pads(out);
    api->joystick(&x, &y);
    x += 1000, y += 1000;
    out[8] = x >> 7, out[9] = x & 127, out[10] = y >> 7, out[11] = y & 127;
    out[12] = *api->buttons >> 7, out[13] = *api->buttons & 127;
    api->keys(out + 14);
    api->knobs(out + 39, out + 47, &rel);  /* knob values land past the 47 bytes sent (out holds 64) */
    return 47;
}

int program(const prog_api_t *a) {
    api = a;
    if (api->version < 2) return 0;  /* needs firmware v9.80 */
    *api->own_input = 1;
    *api->on_message = message;
    int waiting_shown = 0;
    while (!quit && !(*api->buttons & 1u << MPK_BTN_PROG_SELECT)) {
        api->poll();
        if (show) show = 0, api->flush(), waiting_shown = 0;
        int gone = connected && (int32_t)(*api->ticks - last_msg) > 2 * 20000;
        if ((!connected || gone) && !waiting_shown) {  /* waiting for the computer */
            waiting_shown = 1;
            api->fb_clear();
            api->text(0, 4, gone ? "COMPUTER STOPPED" : "REMOTE SCREEN", 1);
            api->text(0, 15, "WAITING FOR PC...", 1);
            api->text(0, 25, "PROG SELECT: EXIT", 1);
            api->flush();
        }
        uint8_t p[8];
        api->pads(p);
        for (int i = 0; i < 8; i++) api->led_level[MPK_LED_PAD0 + i] = p[i] / 4;
    }
    *api->on_message = 0;
    return 0;
}
