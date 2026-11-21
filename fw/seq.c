#include "seq.h"
#include "analog.h"
#include "settings.h"

#define TICK_HZ 20000u
volatile uint8_t seq_arp_on, seq_repeat_on, seq_beat_led;

static inline uint32_t note_on(int ch, int n, int v) { return 0x09u | (0x90u | ch) << 8 | (uint32_t)n << 16 | (uint32_t)v << 24; }
static inline uint32_t note_off(int ch, int n) { return 0x08u | (0x80u | ch) << 8 | (uint32_t)n << 16 | 0x40u << 24; }

/* ---- clock: pulses at tempo * 96 per minute; a step = div_pulses[division] pulses, gate = half a step ---- */
static const uint8_t div_pulses[8] = {96, 64, 48, 32, 24, 16, 12, 8};  /* 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T */
static uint32_t acc, seq_ticks, beat;
static uint16_t pulse, seq_step;

/* ---- arpeggiator state: held notes in press order ---- */
#define MAXH 16
static uint8_t hn[MAXH], hv[MAXH], nh, phys;
static int16_t playing = -1;
static uint32_t rnd = 1;

static void arp_off_note(void) {
    if (playing >= 0) { midi_push(note_off(S.keys_ch, playing)); playing = -1; }
}

static void arp_play(void) {
    if (!nh) return;
    uint8_t n[MAXH], v[MAXH];
    int cnt = nh;
    for (int i = 0; i < cnt; i++) n[i] = hn[i], v[i] = hv[i];
    if (S.arp_mode != ARP_ORDER)  /* sort by pitch (insertion sort, at most 16) */
        for (int i = 1; i < cnt; i++)
            for (int j = i; j > 0 && n[j] < n[j - 1]; j--) {
                uint8_t t = n[j]; n[j] = n[j - 1]; n[j - 1] = t;
                t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
            }
    int L = cnt * (S.arp_oct + 1), p, k;
    switch (S.arp_mode) {
    case ARP_DOWN: p = L - 1 - seq_step % L; break;
    case ARP_EXC: if (L < 2) p = 0; else k = seq_step % (2 * L - 2), p = k < L ? k : 2 * L - 2 - k; break;
    case ARP_INC: k = seq_step % (2 * L), p = k < L ? k : 2 * L - 1 - k; break;
    case ARP_RANDOM: rnd = rnd * 1103515245u + 12345u; p = (rnd >> 16) % L; break;
    default: p = seq_step % L;  /* UP, ORDER */
    }
    int note = n[p % cnt] + 12 * (p / cnt);
    arp_off_note();
    playing = note > 127 ? 127 : note;
    midi_push(note_on(S.keys_ch, playing, v[p % cnt]));
}

/* ---- note repeat: held pads retrigger on every step ---- */
static struct { uint8_t held, sounding, note, vel; } rp[8];

static int any_repeat(void) { for (int i = 0; i < 8; i++) if (rp[i].held) return 1; return 0; }

static void step_start(void) {
    if (seq_arp_on) arp_play();
    if (seq_repeat_on)
        for (int i = 0; i < 8; i++)
            if (rp[i].held) {
                if (rp[i].sounding) midi_push(note_off(S.pads_ch, rp[i].note));
                midi_push(note_on(S.pads_ch, rp[i].note, rp[i].vel));
                rp[i].sounding = 1;
            }
}

static void gate_end(void) {
    arp_off_note();
    for (int i = 0; i < 8; i++)
        if (rp[i].sounding) midi_push(note_off(S.pads_ch, rp[i].note)), rp[i].sounding = 0;
}

/* On external clock the grid belongs to the host (Start resets it), so a first note doesn't restart it. */
static void restart_grid(void) { if (!S.clock_ext) pulse = 0, seq_step = 0; }

static void do_pulse(void);

/* ---- external MIDI clock: 24 per quarter = 4 of our pulses each. Each F8 brings the pulse count to exactly
 * 4 x clocks (steps are multiples of 4 pulses, so they land on a clock); in between, up to 3 pulses are filled in
 * at the measured spacing, never running ahead of the host. ---- */
static uint32_t ext_last, ext_period, last_pulse, ext_done, ext_target;  /* ext_period 0 = not measured yet */
static uint32_t ext_iv16;  /* clock interval in ticks x16, smoothed, for the BPM display */
static uint8_t stopped;

void seq_realtime(uint8_t b) {
    if (!S.clock_ext) return;
    if (b == 0xF8) {
        uint32_t iv = seq_ticks - ext_last;
        ext_last = seq_ticks;
        ext_period = iv < TICK_HZ ? iv / 4 : 0;     /* a gap over 1 s: don't guess the spacing */
        if (ext_period) {  /* smooth USB jitter for the BPM display, but follow a real tempo change at once */
            int32_t d = (int32_t)(iv * 16) - (int32_t)ext_iv16;
            ext_iv16 = !ext_iv16 || d > (int32_t)ext_iv16 / 4 || -d > (int32_t)ext_iv16 / 4 ? iv * 16 : ext_iv16 + d / 8;
        }
        if (stopped) return;
        ext_target += 4;
        while (ext_done < ext_target) ext_done++, do_pulse();
        last_pulse = seq_ticks;
    } else if (b == 0xFA) {                          /* Start: back to the top, first step now */
        stopped = 0, ext_done = ext_target = 0, pulse = 0, seq_step = 0, beat = 0;
        gate_end();
        step_start();
    } else if (b == 0xFB) {                          /* Continue */
        stopped = 0;
    } else if (b == 0xFC) {                          /* Stop: silence arp and repeat notes */
        stopped = 1;
        gate_end();
    }
}

int seq_bpm(void) {
    if (!S.clock_ext) return S.tempo;
    if (!ext_iv16 || seq_ticks - ext_last > TICK_HZ) return 0;  /* no clock in the last second */
    return (TICK_HZ * 60 / 24 * 16 + ext_iv16 / 2) / ext_iv16;  /* 24 clocks per quarter note */
}

void seq_tick(void) {
    seq_ticks++;
    if (seq_arp_on && !S.arp_latch && !phys && nh) nh = 0, arp_off_note();  /* latch switched off with nothing held */
    if (S.clock_ext) {
        if (!stopped && ext_period && ext_done < ext_target + 3 && seq_ticks - last_pulse >= ext_period)
            ext_done++, last_pulse = seq_ticks, do_pulse();
        return;
    }
    acc += (uint32_t)S.tempo * 96;
    if (acc < TICK_HZ * 60) return;
    acc -= TICK_HZ * 60;
    do_pulse();
}

/* Swing lengthens even steps and shortens odd ones by the same amount (pairs keep the grid); gate = % of a step. */
static void do_pulse(void) {
    beat = (beat + 1) % 96;
    seq_beat_led = beat < 12;
    int len = div_pulses[S.arp_div], sw = len * (S.swing - 50) / 50;
    int cur = seq_step & 1 ? len - sw : len + sw;
    int gate = cur * S.gate / 100;
    if (++pulse == (gate < 1 ? 1 : gate)) gate_end();
    if (pulse >= cur) pulse = 0, seq_step++, step_start();
}

void seq_arp_toggle(void) {
    seq_arp_on ^= 1;
    if (!seq_arp_on) arp_off_note(), nh = 0, phys = 0;
}

void arp_key(int note, int vel, int on) {
    if (on) {
        if (S.arp_latch && !phys) nh = 0;  /* latch: a new chord after letting go replaces the old one */
        phys++;
        int fresh = !nh;
        if (nh < MAXH) hn[nh] = note, hv[nh] = vel, nh++;
        if (fresh && !any_repeat()) restart_grid(), arp_play();  /* first note sounds at once, grid starts here */
    } else {
        if (phys) phys--;
        if (!S.arp_latch)
            for (int i = 0; i < nh; i++)
                if (hn[i] == note) {
                    for (int j = i; j < nh - 1; j++) hn[j] = hn[j + 1], hv[j] = hv[j + 1];
                    nh--;
                    break;
                }
        if (!nh) arp_off_note();
    }
}

void repeat_pad(int pad, int note, int vel, int on) {
    if (on) {
        if (!any_repeat() && !(seq_arp_on && nh)) restart_grid();
        if (rp[pad].sounding) midi_push(note_off(S.pads_ch, rp[pad].note));
        rp[pad].held = 1, rp[pad].sounding = 1, rp[pad].note = note, rp[pad].vel = vel;
        midi_push(note_on(S.pads_ch, note, vel));
    } else {
        rp[pad].held = 0;
        if (rp[pad].sounding) midi_push(note_off(S.pads_ch, rp[pad].note)), rp[pad].sounding = 0;
    }
}

/* ---- tap tempo: average of the last up to 3 intervals; a pause over 2 s starts over ---- */
void seq_tap(void) {
    if (S.clock_ext) return;  /* the host sets the tempo */
    static uint32_t last, iv[3];
    static uint8_t n;
    uint32_t dt = seq_ticks - last;
    last = seq_ticks;
    if (dt > 2 * TICK_HZ || dt < TICK_HZ / 5) { n = 0; return; }
    iv[n % 3] = dt;
    n++;
    int k = n < 3 ? n : 3;
    uint32_t sum = 0;
    for (int i = 0; i < k; i++) sum += iv[i];
    uint32_t bpm = TICK_HZ * 60 * k / sum;
    S.tempo = bpm < 40 ? 40 : bpm > 240 ? 240 : bpm;
}
