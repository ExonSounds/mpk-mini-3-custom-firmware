/* Computer side of the emulated USB-MIDI interface, present while the device is configured (like plugging it in):
 *  - an ALSA sequencer client + port named after the USB product string (what DAWs see), and
 *  - a PTY in raw mode carrying the MIDI byte stream, published as RUN_DIR/midi (symlink to the pty slave) next to
 *    RUN_DIR/name (the product string), which mpk3.py / mpk3_cfg.py open like a raw-MIDI device.
 * Device -> computer: USB-MIDI packets -> bytes, written without blocking; dropped while nobody has the pty open
 * (like rawmidi) or the sequencer queue is full. Computer -> device: a reader thread turns bytes (running status,
 * SysEx of any length, realtime anywhere) into USB-MIDI packets for the host's bulk OUT, with back-pressure.
 * A device with a HID interface (stock: the update protocol) also gets RUN_DIR/hid, a pty that works like its
 * hidraw node: write report ID 0 + 32 bytes per output report, read 32 bytes per input report. */
#define _GNU_SOURCE  /* posix_openpt, ptsname */
#include "core.h"
#include "usb.h"
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;  /* everything below except the paths */
static snd_seq_t *seq;
static int seq_port, gen, wake[2];
static struct pty { int fd, hup; char link[600]; } midi = {-1, 0, ""}, hid = {-1, 0, ""};
static snd_midi_event_t *enc, *dec;
static uint32_t ring[1024];  /* computer -> device packets */
static int rhead, rn;
static atomic_int kick_posted;
static char name_path[600];
#define HIDREP 32  /* shortcut: the report size of Akai's HID interface, upgrade if another device needs others */
static uint8_t hring[16][HIDREP], hacc[1 + HIDREP];  /* computer -> device reports; partial write */
static int hhead, hn, hacc_n;

struct parser { uint8_t status, b[3]; int n, need, cin, sysex; };
static struct parser p_pty, p_seq;

/* one MIDI byte from the computer; returns 1 and sets *pkt when a USB-MIDI packet (cable 0) is complete */
static int parse(struct parser *p, uint8_t c, uint32_t *pkt) {
    if (c >= 0xF8) { *pkt = 0x0F | (uint32_t)c << 8; return 1; }  /* realtime: CIN F, even inside SysEx */
    if (c == 0xF0) { p->sysex = 1, p->b[0] = c, p->n = 1, p->status = 0; return 0; }
    if (c == 0xF7) {
        if (!p->sysex) return 0;
        p->b[p->n++] = c, p->sysex = 0;
        *pkt = (uint32_t)(4 + p->n) | p->b[0] << 8 | (p->n > 1 ? p->b[1] << 16 : 0) | (uint32_t)(p->n > 2 ? p->b[2] : 0) << 24;
        p->n = 0;
        return 1;
    }
    if (c >= 0x80) {
        p->sysex = 0, p->n = 1, p->b[0] = c;
        if (c < 0xF0) { p->status = c, p->cin = c >> 4, p->need = (c & 0xE0) == 0xC0 ? 2 : 3; return 0; }
        p->status = 0;  /* system common cancels running status */
        if (c == 0xF6) { *pkt = 0x05 | 0xF6 << 8; p->n = 0; return 1; }
        p->need = c == 0xF2 ? 3 : 2, p->cin = p->need;  /* F1/F3: CIN 2, F2: CIN 3 */
        if (c == 0xF4 || c == 0xF5) p->n = 0;           /* undefined: ignore */
        return 0;
    }
    if (p->sysex) {
        p->b[p->n++] = c;
        if (p->n < 3) return 0;
        *pkt = 0x04 | p->b[0] << 8 | p->b[1] << 16 | (uint32_t)p->b[2] << 24;
        p->n = 0;
        return 1;
    }
    if (!p->n) {  /* running status */
        if (!p->status) return 0;
        p->b[0] = p->status, p->n = 1, p->need = (p->status & 0xE0) == 0xC0 ? 2 : 3, p->cin = p->status >> 4;
    }
    p->b[p->n++] = c;
    if (p->n < p->need) return 0;
    *pkt = (uint32_t)p->cin | p->b[0] << 8 | p->b[1] << 16 | (uint32_t)(p->need > 2 ? p->b[2] : 0) << 24;
    p->n = 0;
    return 1;
}

static void kick(void *ctx) { (void)ctx; kick_posted = 0; usbhost_kick(); }
static int feed(struct parser *p, const uint8_t *d, int n) {  /* caller holds mu; returns packets queued */
    int k = 0;
    uint32_t pkt;
    for (int i = 0; i < n; i++)
        if (parse(p, d[i], &pkt) && rn < (int)(sizeof ring / 4)) ring[(rhead + rn++) % 1024] = pkt, k++;
    return k;
}

static void *reader(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd pf[8] = {{wake[0], POLLIN, 0}};
        int n = 1, imidi = -1, ihid = -1, iseq = -1, room;
        pthread_mutex_lock(&mu);
        int g = gen;
        room = 1024 - rn >= 256;
        /* ring full / nobody on a pty (it polls as hung up): look again soon */
        int timeout = !room || hn == 16 || (midi.fd >= 0 && midi.hup) || (hid.fd >= 0 && hid.hup) ? 20 : 1000;
        if (room && midi.fd >= 0 && !midi.hup) imidi = n, pf[n++] = (struct pollfd){midi.fd, POLLIN, 0};
        if (hn < 16 && hid.fd >= 0 && !hid.hup) ihid = n, pf[n++] = (struct pollfd){hid.fd, POLLIN, 0};
        if (room && seq) iseq = n, n += snd_seq_poll_descriptors(seq, pf + n, 4, POLLIN);
        pthread_mutex_unlock(&mu);
        poll(pf, n, timeout);
        if (pf[0].revents) { char b[64]; if (read(wake[0], b, sizeof b) < 0) {} }
        int k = 0;
        uint8_t b[256];
        pthread_mutex_lock(&mu);
        midi.hup = hid.hup = 0;
        if (g == gen && imidi > 0) {
            midi.hup = (pf[imidi].revents & (POLLHUP | POLLIN)) == POLLHUP;
            ssize_t r = pf[imidi].revents & POLLIN ? read(midi.fd, b, sizeof b) : 0;
            if (r > 0) k += feed(&p_pty, b, (int)r);
        }
        if (g == gen && ihid > 0) {  /* one report per 1 + HIDREP bytes; the leading report ID 0 is dropped */
            hid.hup = (pf[ihid].revents & (POLLHUP | POLLIN)) == POLLHUP;
            size_t want = (size_t)(16 - hn) * sizeof hacc - hacc_n;  /* never more than the ring can take */
            ssize_t r = pf[ihid].revents & POLLIN ? read(hid.fd, b, want < sizeof b ? want : sizeof b) : 0;
            for (ssize_t i = 0; i < r; i++) {
                hacc[hacc_n++] = b[i];
                if (hacc_n == sizeof hacc) memcpy(hring[(hhead + hn++) % 16], hacc + 1, HIDREP), hacc_n = 0, k++;
            }
        }
        if (g == gen && iseq > 0 && seq) {
            snd_seq_event_t *ev;
            while (1024 - rn >= 256 && snd_seq_event_input(seq, &ev) >= 0 && ev) {
                static uint8_t sx[4096];
                long r = snd_midi_event_decode(dec, sx, sizeof sx, ev);
                if (r > 0) k += feed(&p_seq, sx, (int)r);
            }
        }
        pthread_mutex_unlock(&mu);
        if (k && !atomic_exchange(&kick_posted, 1)) post(kick, 0);
    }
    return 0;
}

int hid_to_device(uint8_t *buf) {
    pthread_mutex_lock(&mu);
    int n = hn ? HIDREP : 0;
    if (n) memcpy(buf, hring[hhead], HIDREP), hhead = (hhead + 1) % 16, hn--;
    pthread_mutex_unlock(&mu);
    return n;
}

static int listening(struct pty *p) {  /* a pty nobody has open drops data, like a closed rawmidi/hidraw node */
    struct pollfd pf = {p->fd, POLLOUT, 0};
    return p->fd >= 0 && poll(&pf, 1, 0) == 1 && !(pf.revents & POLLHUP);
}

void hid_from_device(const uint8_t *buf, int n) {
    pthread_mutex_lock(&mu);
    if (listening(&hid) && write(hid.fd, buf, n) < 0) {}
    pthread_mutex_unlock(&mu);
}

int midi_to_device(uint32_t *pkts, int max) {
    pthread_mutex_lock(&mu);
    int n = rn < max ? rn : max;
    for (int i = 0; i < n; i++) pkts[i] = ring[(rhead + i) % 1024];
    rhead = (rhead + n) % 1024, rn -= n;
    pthread_mutex_unlock(&mu);
    return n;
}

void midi_from_device(uint32_t pkt) {
    static const int8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};  /* bytes per CIN */
    int n = len[pkt & 15];
    uint8_t b[3] = {pkt >> 8, pkt >> 16, pkt >> 24};
    if (!n) return;
    pthread_mutex_lock(&mu);
    if (listening(&midi) && write(midi.fd, b, n) < 0) {}  /* full: drop */
    for (int i = 0; seq && i < n; i++) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        if (snd_midi_event_encode_byte(enc, b[i], &ev) != 1) continue;
        snd_seq_ev_set_source(&ev, seq_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq, &ev);  /* nonblocking: -EAGAIN drops it */
    }
    pthread_mutex_unlock(&mu);
}

/* a raw-mode pty, published as a symlink to its slave; returns the master fd or -1 */
static int pty_open(struct pty *p) {
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    const char *slave = m >= 0 && !grantpt(m) && !unlockpt(m) ? ptsname(m) : 0;
    int s = slave ? open(slave, O_RDWR | O_NOCTTY) : -1;
    if (s < 0) {
        LOG("USB: no pty for %s (%s)", p->link, strerror(errno));
        if (m >= 0) close(m);
        return -1;
    }
    struct termios t;
    tcgetattr(s, &t);
    cfmakeraw(&t);
    tcsetattr(s, TCSANOW, &t);  /* stays raw while the master is open */
    close(s);
    unlink(p->link);
    if (symlink(slave, p->link)) LOG("USB: %s: %s", p->link, strerror(errno));
    LOG("USB: %s -> %s", p->link, slave);
    return m;
}

void midi_attach(const char *product, int has_hid) {
    midi_detach();
    int m = pty_open(&midi), h = has_hid ? pty_open(&hid) : -1;
    snd_seq_t *sq = 0;
    int port = -1;
    if (snd_seq_open(&sq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) == 0) {
        char pn[80];
        snprintf(pn, sizeof pn, "%s MIDI 1", product);
        snd_seq_set_client_name(sq, product);
        port = snd_seq_create_simple_port(sq, pn,
                                          SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_DUPLEX |
                                              SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE |
                                              SND_SEQ_PORT_TYPE_PORT);
        if (port < 0) snd_seq_close(sq), sq = 0;
    }
    if (!sq) LOG("MIDI: no ALSA sequencer port");
    pthread_mutex_lock(&mu);
    midi.fd = m, hid.fd = h, midi.hup = hid.hup = 0, seq = sq, seq_port = port, gen++, rn = hn = hacc_n = 0;
    snd_midi_event_new(256, &enc);
    snd_midi_event_new(16, &dec);
    snd_midi_event_no_status(dec, 1);
    memset(&p_pty, 0, sizeof p_pty), memset(&p_seq, 0, sizeof p_seq);
    pthread_mutex_unlock(&mu);
    if (write(wake[1], "", 1) < 0) {}
    char tmp[620];
    snprintf(tmp, sizeof tmp, "%s.tmp", name_path);
    FILE *f = fopen(tmp, "w");
    if (f) fprintf(f, "%s\n", product), fclose(f), rename(tmp, name_path);
    LOG("MIDI: \"%s\", ALSA seq %d:%d", product, sq ? snd_seq_client_id(sq) : -1, port);
}

void midi_detach(void) {
    pthread_mutex_lock(&mu);
    int was = midi.fd >= 0 || hid.fd >= 0 || seq;
    for (struct pty *p = &midi; p; p = p == &midi ? &hid : 0)
        if (p->fd >= 0) close(p->fd), p->fd = -1, unlink(p->link);
    if (seq) snd_seq_close(seq), seq = 0;
    if (enc) snd_midi_event_free(enc), enc = 0;
    if (dec) snd_midi_event_free(dec), dec = 0;
    gen++, rn = hn = 0;
    pthread_mutex_unlock(&mu);
    if (write(wake[1], "", 1) < 0) {}
    if (was) unlink(name_path);
}

void midi_bridge_init(void) {
    snprintf(midi.link, sizeof midi.link, "%s/midi", emu_run_dir);
    snprintf(hid.link, sizeof hid.link, "%s/hid", emu_run_dir);
    snprintf(name_path, sizeof name_path, "%s/name", emu_run_dir);
    unlink(midi.link), unlink(hid.link), unlink(name_path);  /* left over from an emulator that was killed */
    if (pipe(wake)) { perror("pipe"); exit(1); }
    fcntl(wake[0], F_SETFL, O_NONBLOCK), fcntl(wake[1], F_SETFL, O_NONBLOCK);
    pthread_t t;
    pthread_create(&t, 0, reader, 0);
    pthread_detach(t);
    atexit(midi_detach);
}
