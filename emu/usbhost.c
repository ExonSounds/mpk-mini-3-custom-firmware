/* The computer's USB host port, as Linux drives a full-speed device: debounce after connect, bus reset,
 * GET_DESCRIPTOR(device, 64) at address 0, a second reset, SET_ADDRESS, device/config/string descriptors,
 * SET_CONFIGURATION, then (for a HID interface, as usbhid does) SET_IDLE and the report descriptor. Then frames:
 * SOF every 1 ms, bulk MIDI IN/OUT polled, HID interrupt IN/OUT every bInterval frames (to the hid pty).
 * Transactions run one per 50 us slot (about the full-speed bus rate) while there is work, else one per frame.
 * Everything is timed in emulated cycles. */
#include "core.h"
#include "usb.h"
#include <string.h>

#define FRAME (F_CPU / 1000)
#define SLOT (F_CPU / 20000)
#define MS(x) ((uint64_t)(x) * FRAME)

enum { OFF, DEBOUNCE, RESET, ENUM, RUN, FAILED };
static int state, step;
static unsigned frame;
static uint64_t next_sof, idle_until;

static struct {
    int active, stage, len, pos;
    uint8_t s[8], d[1024 + 64];
    uint64_t deadline;
} ctl;
static int mps0 = 64;

static uint8_t dev[18], cfg[1024];
static int cfg_len, langid = 0x0409;
static char product[64];
static int midi_in, midi_out, midi_out_mps, hid_if = -1, hid_in, hid_out, hid_interval = 1, hid_rlen;
static uint32_t outpk[16];  /* bulk OUT packet being sent (kept until the device ACKs it) */
static int outn;
static unsigned hid_last;
static uint8_t hidrep[64];  /* HID output report being sent */
static int hidn;

static void tick(void *ctx);

static void req(int type, int r, int value, int index, int len) {
    uint8_t s[8] = {type, r, value, value >> 8, index, index >> 8, len, len >> 8};
    memcpy(ctl.s, s, 8);
    ctl.active = 1, ctl.stage = 0, ctl.len = len, ctl.pos = 0, ctl.deadline = now() + MS(5000);  /* Linux: 5 s */
}

static void parse_cfg(void) {
    int cls = 0, sub = 0, ifn = 0;
    midi_in = midi_out = hid_in = hid_out = 0, hid_if = -1;
    for (int i = 0; i + 2 <= cfg_len && cfg[i] >= 2; i += cfg[i]) {
        const uint8_t *d = cfg + i;
        if (d[1] == 4) ifn = d[2], cls = d[5], sub = d[6];
        if (d[1] == 0x21 && cls == 3 && d[0] >= 9) hid_if = ifn, hid_rlen = d[7] | d[8] << 8;
        if (d[1] != 5) continue;
        int addr = d[2], type = d[3] & 3, mps = d[4] | d[5] << 8;
        if (cls == 1 && sub == 3 && type == 2) {
            if (addr & 0x80) midi_in = addr & 15;
            else midi_out = addr & 15, midi_out_mps = mps > 64 ? 64 : mps;
        }
        if (cls == 3 && type == 3 && (addr & 0x80)) hid_in = addr & 15, hid_interval = d[6] ? d[6] : 1;
        if (cls == 3 && type == 3 && !(addr & 0x80)) hid_out = addr & 15;
    }
}

static void string(char *out, int size) {  /* UTF-16LE descriptor in ctl.d -> ASCII */
    int n = 0;
    for (int i = 2; i + 1 < ctl.pos && i + 1 < ctl.d[0] && n < size - 1; i += 2)
        out[n++] = ctl.d[i + 1] || ctl.d[i] >= 128 || ctl.d[i] < 32 ? '?' : ctl.d[i];
    out[n] = 0;
}

static void fail(const char *what) {
    LOG("USB host: %s failed, device not enumerated", what);
    panel_status("USB enumeration failed (%s)", what);
    state = FAILED;
}

/* start the transfer of the current enumeration step, or act on a step that has none */
static void enum_next(void) {
    for (;;) switch (step) {
        case 0: req(0x80, 6, 0x0100, 0, 64); return;
        case 1: step++, state = DEBOUNCE; return;  /* second reset, like Linux */
        case 2: req(0x00, 5, 1, 0, 0); return;    /* SET_ADDRESS 1 */
        case 3: req(0x80, 6, 0x0100, 0, 18); return;
        case 4: req(0x80, 6, 0x0200, 0, 9); return;
        case 5: req(0x80, 6, 0x0200, 0, cfg_len); return;
        case 6: req(0x80, 6, 0x0300, 0, 255); return;
        case 7: case 8: case 9: {
            int idx = dev[15 + step - 7];  /* iProduct, iManufacturer, iSerialNumber */
            if (idx) { req(0x80, 6, 0x0300 | idx, langid, 255); return; }
            step++;
            break;
        }
        case 10: req(0x00, 9, cfg[5], 0, 0); return;  /* SET_CONFIGURATION */
        case 11: if (hid_if >= 0) { req(0x21, 0x0A, 0, hid_if, 0); return; } step++; break;  /* SET_IDLE 0 */
        case 12: if (hid_if >= 0 && hid_rlen) { req(0x81, 6, 0x2200, hid_if, hid_rlen > 1024 ? 1024 : hid_rlen); return; } step++; break;
        default:
            state = RUN, outn = hidn = 0;
            LOG("USB host: configured \"%s\" (%04x:%04x), MIDI IN ep %d OUT ep %d, HID IN ep %d OUT ep %d", product,
                dev[8] | dev[9] << 8, dev[10] | dev[11] << 8, midi_in, midi_out, hid_in, hid_out);
            midi_attach(product, hid_in && hid_out);
            panel_status("USB enumerated: %s", product);  /* after the ports exist: tests wait for this */
            return;
        }
}

static void ctl_done(int ok) {
    ctl.active = 0;
    int got = ctl.pos;
    switch (step) {
    case 0: if (!ok || got < 8) return fail("GET_DESCRIPTOR(device)"); mps0 = ctl.d[7] ? ctl.d[7] : 64; break;
    case 2: if (!ok) return fail("SET_ADDRESS"); idle_until = now() + MS(2); break;
    case 3: if (!ok || got < 18) return fail("GET_DESCRIPTOR(device)"); memcpy(dev, ctl.d, 18); break;
    case 4:
        if (!ok || got < 9) return fail("GET_DESCRIPTOR(config)");
        cfg_len = ctl.d[2] | ctl.d[3] << 8;
        if (cfg_len > (int)sizeof cfg) cfg_len = sizeof cfg;
        break;
    case 5: if (!ok) return fail("GET_DESCRIPTOR(config)"); memcpy(cfg, ctl.d, cfg_len = got); parse_cfg(); break;
    case 6: if (ok && got >= 4) langid = ctl.d[2] | ctl.d[3] << 8; break;
    case 7: if (ok) string(product, sizeof product); break;
    case 10: if (!ok) return fail("SET_CONFIGURATION"); break;
    }  /* strings and HID requests may fail, as Linux tolerates */
    step++;
}

static int ctl_step(void) {
    int n, r;
    if (now() > ctl.deadline) { ctl_done(0); return 1; }
    switch (ctl.stage) {
    case 0:
        if (usb_dev_setup(ctl.s) == USB_ACK) ctl.stage = ctl.len ? 1 : 2;
        return 1;
    case 1:
        r = usb_dev_in(0, ctl.d + ctl.pos, &n);
        if (r == USB_STALL) ctl_done(0);
        if (r != USB_ACK) return 1;
        ctl.pos += n;
        if (ctl.pos > ctl.len) ctl.pos = ctl.len;
        if (n < mps0 || ctl.pos >= ctl.len) ctl.stage = 2;
        return 1;
    default:  /* status: zero-length OUT after IN data, zero-length IN otherwise */
        r = ctl.len ? usb_dev_out(0, 0, 0) : usb_dev_in(0, ctl.d, &n);
        if (r != USB_NAK) ctl_done(r == USB_ACK);
        return 1;
    }
}

static int run_polls(void) {
    uint8_t b[64];
    int n, busy = 0;
    if (midi_in && usb_dev_in(midi_in, b, &n) == USB_ACK && n) {
        for (int i = 0; i + 4 <= n; i += 4) midi_from_device(b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
        busy = 1;
    }
    if (midi_out && !outn) outn = midi_to_device(outpk, midi_out_mps / 4);
    if (outn) {
        for (int i = 0; i < outn; i++) memcpy(b + 4 * i, &outpk[i], 4);
        if (usb_dev_out(midi_out, b, 4 * outn) == USB_ACK) outn = 0;
        busy = 1;
    }
    if (hid_in && frame != hid_last && frame % hid_interval == 0) {  /* interrupt endpoints: once per interval */
        hid_last = frame;
        if (usb_dev_in(hid_in, b, &n) == USB_ACK && n) hid_from_device(b, n);
        if (hid_out && !hidn) hidn = hid_to_device(hidrep);
        if (hidn && usb_dev_out(hid_out, hidrep, hidn) == USB_ACK) hidn = 0;
    }
    return busy;
}

static void tick(void *ctx) {
    (void)ctx;
    uint64_t t = now();
    switch (state) {
    case DEBOUNCE: usb_dev_bus_reset(1), state = RESET, ev_at(t + MS(10), tick, 0); return;
    case RESET: usb_dev_bus_reset(0), state = ENUM, next_sof = t, idle_until = t + MS(10); break;
    case ENUM: case RUN: break;
    default: return;
    }
    if (t >= next_sof) usb_dev_sof(frame = (frame + 1) & 0x7FF), next_sof += FRAME;
    int busy = 1;
    if (t < idle_until) busy = 0;
    else if (ctl.active) ctl_step();
    else if (state == ENUM) enum_next();
    else busy = run_polls();
    uint64_t nx = busy ? t + SLOT : next_sof;
    if (idle_until > t && idle_until < nx) nx = idle_until;
    ev_at(nx, tick, 0);
}

void usbhost_connect(int on) {
    if (state == RUN) midi_detach(), panel_status("USB disconnected");
    ev_cancel(tick, 0);
    state = on ? DEBOUNCE : OFF, step = 0, ctl.active = 0, mps0 = 64, product[0] = 0;
    if (on) ev_at(now() + MS(100), tick, 0);  /* hub debounce */
}

void usbhost_kick(void) {
    if (state == RUN) ev_at(now(), tick, 0);
}
