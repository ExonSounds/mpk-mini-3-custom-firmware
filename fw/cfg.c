#include "cfg.h"
#include "analog.h"
#include "board.h"
#include "menu.h"
#include "prog.h"
#include "prog_api.h"
#include "settings.h"

#ifndef FW_VERSION
#define FW_VERSION 0
#endif
/* HELLO sends the version as two 7-bit bytes: 9.80 (0x0980) went out as 9.00. Pick versions like 10.00-10.70. */
_Static_assert((FW_VERSION & 0x80) == 0 && FW_VERSION >> 8 < 0x80, "FW_VERSION bytes must be < 0x80");

static const uint8_t hdr[4] = {0xF0, 0x7D, 0x4D, 0x4B};
#define MAXLEN (6 + 2 * sizeof(settings_t))  /* header + cmd + nibbles + F7 */

/* ---- receive: reassemble SysEx from USB-MIDI packets (USB interrupt), hand complete messages to the main loop ---- */
static uint8_t rx[MAXLEN + 4];
static volatile uint8_t cmd_buf[MAXLEN + 4];
static unsigned rx_len;
static volatile unsigned cmd_len;  /* != 0: a message waits for cfg_poll() */

void cfg_rx(uint32_t pkt) {
    static const uint8_t count[16] = {[4] = 3, [5] = 1, [6] = 2, [7] = 3};
    int cin = pkt & 0xF, n = count[cin];
    if (!n) return;
    for (int i = 0; i < n; i++) {
        uint8_t b = pkt >> (8 * (i + 1));
        if (b == 0xF0) rx_len = 0;
        if (rx_len < sizeof rx) rx[rx_len++] = b;
        else rx_len = sizeof rx + 1;  /* too long: drop when it ends */
        if (b == 0xF7) {
            if (rx_len <= sizeof rx && !cmd_len) {
                for (unsigned j = 0; j < rx_len; j++) cmd_buf[j] = rx[j];
                cmd_len = rx_len;
            }
            rx_len = 0;
            return;
        }
    }
}

/* ---- send: split a SysEx message into USB-MIDI packets ---- */
static void send_sysex(const uint8_t *m, unsigned len) {
    for (unsigned i = 0; i < len; i += 3) {
        unsigned n = len - i < 3 ? len - i : 3;
        uint32_t cin = i + n < len ? 0x4 : 0x4 + n;  /* 4 = start/continue, 5/6/7 = ends with 1/2/3 bytes */
        uint32_t p = cin;
        for (unsigned j = 0; j < n; j++) p |= (uint32_t)m[i + j] << (8 * (j + 1));
        midi_queue(p);
    }
}

static void reply(uint8_t cmd, const uint8_t *data, unsigned n) {
    uint8_t m[MAXLEN];
    unsigned len = 0;
    for (int i = 0; i < 4; i++) m[len++] = hdr[i];
    m[len++] = cmd | 0x40;
    for (unsigned i = 0; i < n && len < sizeof m - 1; i++) m[len++] = data[i];
    m[len++] = 0xF7;
    send_sysex(m, len);
}

static void status(uint8_t cmd, uint8_t s) { reply(cmd, &s, 1); }

void cfg_poll(void) {
    unsigned len = cmd_len;
    if (!len) return;
    uint8_t m[MAXLEN + 4];
    for (unsigned i = 0; i < len; i++) m[i] = cmd_buf[i];
    cmd_len = 0;
    if (len < 6 || m[1] != hdr[1] || m[2] != hdr[2] || m[3] != hdr[3]) return;  /* not ours */
    uint8_t cmd = m[4];
    const uint8_t *data = m + 5;
    unsigned n = len - 6;
    switch (cmd) {
    case 0x01: {
        uint8_t info[4] = {FW_VERSION >> 8 & 0x7F, FW_VERSION & 0x7F, SETTINGS_LAYOUT, sizeof(settings_t)};
        reply(cmd, info, 4);
        break;
    }
    case 0x02: {
        uint8_t nib[2 * sizeof(settings_t)];
        const uint8_t *s = (const uint8_t *)&S;
        for (unsigned i = 0; i < sizeof S; i++) nib[2 * i] = s[i] >> 4, nib[2 * i + 1] = s[i] & 15;
        reply(cmd, nib, sizeof nib);
        break;
    }
    case 0x03: {
        if (n != 2 * sizeof(settings_t)) { status(cmd, 1); break; }
        settings_t t;
        uint8_t *d = (uint8_t *)&t;
        for (unsigned i = 0; i < sizeof t; i++) d[i] = (data[2 * i] & 15) << 4 | (data[2 * i + 1] & 15);
        if (!settings_valid(&t)) { status(cmd, 2); break; }
        irq_disable();
        for (unsigned i = 0; i < sizeof t; i++) ((uint8_t *)&S)[i] = d[i];  /* no libc memcpy on target */
        irq_enable();
        menu_dirty = 1;
        status(cmd, 0);
        break;
    }
    case 0x04: status(cmd, settings_save() ? 0 : 2); break;
    case 0x05:
        irq_disable();
        settings_defaults();
        irq_enable();
        menu_dirty = 1;
        status(cmd, 0);
        break;
    case 0x06: {  /* IMAGE_WRITE: offset hi, lo (7 bits each), then 64 bytes as nibbles; save with SAVE */
        unsigned off = n >= 2 ? (unsigned)(data[0] << 7 | data[1]) : 512;
        if (n != 2 + 128 || off > 512 - 64) { status(cmd, 1); break; }
        for (unsigned i = 0; i < 64; i++) idle_image[off + i] = (data[2 + 2 * i] & 15) << 4 | (data[3 + 2 * i] & 15);
        idle_image_set = 1, image_dirty = 1;
        status(cmd, 0);
        break;
    }
    case 0x07: {  /* IMAGE_READ: offset hi, lo -> set flag, 64 bytes as nibbles */
        unsigned off = n == 2 ? (unsigned)(data[0] << 7 | data[1]) : 512;
        if (off > 512 - 64) { status(cmd, 1); break; }
        uint8_t r[1 + 128];
        r[0] = idle_image_set;
        for (unsigned i = 0; i < 64; i++) r[1 + 2 * i] = idle_image[off + i] >> 4, r[2 + 2 * i] = idle_image[off + i] & 15;
        reply(cmd, r, sizeof r);
        break;
    }
    case 0x08:    /* IMAGE_CLEAR: back to the built-in logo; save with SAVE */
        idle_image_set = 0, image_dirty = 1;
        status(cmd, 0);
        break;
    case 0x09:    /* PROG_WRITE: block hi, lo (64-byte blocks, 7 bits each), 64 bytes as nibbles */
        if (n != 2 + 128) { status(cmd, 1); break; }
        status(cmd, prog_write((unsigned)(data[0] << 7 | data[1]) * 64, data + 2));
        break;
    case 0x0A:    /* PROG_RUN: size, sum16 (3 x 7 bits each); runs it once this reply is out */
        if (n != 6) { status(cmd, 1); break; }
        status(cmd, prog_start((unsigned)data[0] << 14 | data[1] << 7 | data[2],
                               (unsigned)data[3] << 14 | data[4] << 7 | data[5]));
        break;
    case 0x0B: {  /* PROG_MSG: any 7-bit bytes to the running program -> its answer (empty: no program listens) */
        uint8_t out[PROG_REPLY_MAX];
        reply(cmd, out, prog_message(data, n, out));
        break;
    }
    default: status(cmd, 0x7F);
    }
}
