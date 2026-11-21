#include "prog.h"
#include "analog.h"
#include "board.h"
#include "cfg.h"
#include "prog_api.h"
#include "ui.h"

#ifdef __arm__
#define REGION ((uint8_t *)PROG_BASE)
#else
static uint8_t prog_host_ram[PROG_SIZE];  /* host test (make check) */
#define REGION prog_host_ram
#endif

volatile uint8_t prog_running, prog_own_input, prog_run_req;
static uint32_t prog_entry;

/* provided by main.c */
void midi_flush(void);
void keys_level(uint8_t vel[25]);

static uint32_t word(unsigned off) {  /* little-endian, so the check runs the same on the host */
    return REGION[off] | REGION[off + 1] << 8 | REGION[off + 2] << 16 | (uint32_t)REGION[off + 3] << 24;
}

uint8_t prog_write(unsigned off, const uint8_t *nib) {
    if (prog_running || prog_run_req) return 3;  /* never overwrite code that is running */
    if (off > PROG_SIZE - 64) return 1;
    for (unsigned i = 0; i < 64; i++) REGION[off + i] = (nib[2 * i] & 15) << 4 | (nib[2 * i + 1] & 15);
    return 0;
}

uint8_t prog_start(unsigned size, unsigned sum16) {
    if (prog_running || prog_run_req) return 3;
    if (size < 8 || size > PROG_SIZE) return 1;
    unsigned sum = 0;
    for (unsigned i = 0; i < size; i++) sum += REGION[i];
    uint32_t entry = word(4);
    /* entry must be Thumb code (bit 0 set) inside what was uploaded */
    if ((sum & 0xFFFF) != sum16 || word(0) != PROG_MAGIC || !(entry & 1) || entry < PROG_BASE + 8 ||
        entry >= PROG_BASE + size)
        return 2;
    prog_entry = entry;
    prog_run_req = 1;
    return 0;
}

static void poll(void) { midi_flush(); cfg_poll(); }

static prog_msg_fn on_message;

unsigned prog_message(const uint8_t *in, unsigned n, uint8_t *out) {
    if (!prog_running || !on_message || n > PROG_MSG_MAX) return 0;
    unsigned r = on_message(in, n, out);
    if (r > PROG_REPLY_MAX) r = PROG_REPLY_MAX;
    for (unsigned i = 0; i < r; i++) out[i] &= 0x7F;  /* keep the SysEx valid whatever the program wrote */
    return r;
}

static const prog_api_t api = {
    .version = PROG_API_VERSION, .ticks = &ticks, .poll = poll,
    .own_input = &prog_own_input, .keys = keys_level, .pads = pads_level, .knobs = knob_state, .joystick = joy_state,
    .buttons = &ui_held, .midi_send = midi_queue, .led_level = ui_led_level,
    .fb = fb, .fb_clear = fb_clear, .px = px, .rect = rect, .text = text, .flush = oled_flush,
    .on_message = &on_message,
};

void prog_exec(void) {
    prog_run_req = 0;
    midi_flush();  /* the RUN reply */
    prog_own_input = 0;
    on_message = 0;
    prog_running = 1;
    ui_leds_off = 0;
    oled_power(1);
#ifdef __arm__
    ((int (*)(const prog_api_t *))prog_entry)(&api);
#else
    (void)api;
#endif
    prog_running = 0;
    prog_own_input = 0;
    on_message = 0;
    ui_leds_reset();  /* firmware LED state back, program's LEDs off */
}
