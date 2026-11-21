/* Front-panel socket: the GUI (panel.py) and tests talk to the core over a Unix stream socket, one text line per
 * message. Several clients may connect; all get every output line.
 *
 * core -> client
 *   hello mpk3emu 1
 *   oled ON CONTRAST INVERT HEX     visible 128x32 picture, 512 bytes as 1024 hex digits in SSD1306 page order
 *                                  (byte page*128+x, bit y%8, 1 = lit before INVERT is applied); ON/INVERT 0|1
 *   leds HEX                       24 LED brightnesses 0-255 (48 hex digits), index as in firmware/NOTES.md
 *   status TEXT                    human-readable state changes (USB enumerated, ...); the last one is replayed on connect
 *   sync N                         answer to "sync N" once every earlier command has been applied
 *   peek ADDR HEX                  answer to "peek": LEN bytes of memory from ADDR (decimal), as hex digits
 * client -> core
 *   key K V          key 0-24 (0 = lowest C); V 1-127 = press with that velocity, 0 = release
 *   button B D       button 0-9 (NOTES.md numbering: 0 PROG SELECT ... 9 NOTE REPEAT); D 1 = down, 0 = up
 *   pad P F          pad 0-7 (= pads 1-8, Akai numbering); F 0-127 force, 0 = released
 *   knob N D         knob 0-7 (= K1-K8) turned by D degrees (signed; + = clockwise)
 *   joy X Y          joystick, X and Y -1000..1000 (0 = centre, + = right / up)
 *   pedal D          sustain pedal 1 = down
 *   power            power cycle (inputs held stay held: hold PAD BANK + PROG SELECT for update mode)
 *   sync N           reply "sync N" after everything before it was applied
 *   quit             stop the emulator
 *   peek ADDR LEN    debug/tests: read LEN (1-512) bytes of emulated memory at ADDR (decimal or 0x hex)
 */
#include "core.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* inputs: implemented by the input models (inputs.c); these weak defaults only log */
__attribute__((weak)) void input_key(int key, int vel) { LOG("input_key %d %d (no model)", key, vel); }
__attribute__((weak)) void input_button(int b, int down) { LOG("input_button %d %d (no model)", b, down); }
__attribute__((weak)) void input_pad(int pad, int force) { LOG("input_pad %d %d (no model)", pad, force); }
__attribute__((weak)) void input_knob(int knob, int degrees) { LOG("input_knob %d %d (no model)", knob, degrees); }
__attribute__((weak)) void input_joystick(int x, int y) { LOG("input_joystick %d %d (no model)", x, y); }
__attribute__((weak)) void input_pedal(int down) { LOG("input_pedal %d (no model)", down); }
void core_quit(void);

#define NCLI 8
#define OUTMAX (1 << 20)
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int fd; char *out; size_t n; char in[4096]; size_t inn; } cli[NCLI];
static int lfd = -1, wake[2];

static char cur_oled[1100], cur_leds[64], cur_status[520];  /* latest lines, for clients that connect later */
static size_t cur_oled_n, cur_leds_n, cur_status_n;

static void send_all(const char *line, size_t len) {  /* any thread */
    pthread_mutex_lock(&mu);
    if (!strncmp(line, "oled ", 5)) memcpy(cur_oled, line, len), cur_oled_n = len;
    if (!strncmp(line, "leds ", 5)) memcpy(cur_leds, line, len), cur_leds_n = len;
    if (!strncmp(line, "status ", 7) && len <= sizeof cur_status) memcpy(cur_status, line, len), cur_status_n = len;
    for (int i = 0; i < NCLI; i++) {
        if (cli[i].fd < 0 || cli[i].n + len > OUTMAX) continue;  /* slow client: drop this line */
        memcpy(cli[i].out + cli[i].n, line, len);
        cli[i].n += len;
    }
    pthread_mutex_unlock(&mu);
    if (write(wake[1], "", 1) < 0) {}
}

void panel_status(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + 7, sizeof b - 8, fmt, ap);
    va_end(ap);
    memcpy(b, "status ", 7);
    n = n + 7 > (int)sizeof b - 2 ? (int)sizeof b - 2 : n + 7;
    b[n++] = '\n';
    send_all(b, n);
    LOG("%.*s", n - 8, b + 7);
}

static const char hx[] = "0123456789abcdef";
static uint8_t last_fb[512], last_on = 2, last_con, last_inv;
void panel_frame(const uint8_t fb[512], int on, int contrast, int invert) {
    if (on == last_on && contrast == last_con && invert == last_inv && !memcmp(fb, last_fb, 512)) return;
    memcpy(last_fb, fb, 512), last_on = on, last_con = contrast, last_inv = invert;
    char b[1100];
    int n = sprintf(b, "oled %d %d %d ", on != 0, contrast, invert != 0);
    for (int i = 0; i < 512; i++) b[n++] = hx[fb[i] >> 4], b[n++] = hx[fb[i] & 15];
    b[n++] = '\n';
    send_all(b, n);
}

static uint8_t last_led[24];
static int led_sent;
void panel_leds(const uint8_t level[24]) {
    if (led_sent && !memcmp(level, last_led, 24)) return;
    memcpy(last_led, level, 24), led_sent = 1;
    char b[64];
    int n = sprintf(b, "leds ");
    for (int i = 0; i < 24; i++) b[n++] = hx[level[i] >> 4], b[n++] = hx[level[i] & 15];
    b[n++] = '\n';
    send_all(b, n);
}

/* ---- commands: parsed on the socket thread, applied on the emulator thread ---- */
typedef struct { char verb[12]; int a, b; } cmd_t;
static void apply(void *ctx) {
    cmd_t *c = ctx;
    if (!strcmp(c->verb, "key") && c->a >= 0 && c->a < 25) input_key(c->a, c->b < 0 ? 0 : c->b > 127 ? 127 : c->b);
    else if (!strcmp(c->verb, "button") && c->a >= 0 && c->a < 10) input_button(c->a, c->b != 0);
    else if (!strcmp(c->verb, "pad") && c->a >= 0 && c->a < 8) input_pad(c->a, c->b < 0 ? 0 : c->b > 127 ? 127 : c->b);
    else if (!strcmp(c->verb, "knob") && c->a >= 0 && c->a < 8) input_knob(c->a, c->b);
    else if (!strcmp(c->verb, "joy")) input_joystick(c->a < -1000 ? -1000 : c->a > 1000 ? 1000 : c->a, c->b < -1000 ? -1000 : c->b > 1000 ? 1000 : c->b);
    else if (!strcmp(c->verb, "pedal")) input_pedal(c->a != 0);
    else if (!strcmp(c->verb, "power")) request_reset("power cycle");
    else if (!strcmp(c->verb, "quit")) core_quit();
    else if (!strcmp(c->verb, "peek") && c->b > 0 && c->b <= 512) {
        uint8_t m[512];
        char b[1100];
        int n = sprintf(b, "peek %u ", (unsigned)c->a), ok = uc_mem_read(uc, (uint32_t)c->a, m, c->b) == UC_ERR_OK;
        for (int i = 0; ok && i < c->b; i++) b[n++] = hx[m[i] >> 4], b[n++] = hx[m[i] & 15];
        b[n++] = '\n';
        send_all(b, n);
    }
    else if (!strcmp(c->verb, "sync")) { char b[40]; int n = sprintf(b, "sync %d\n", c->a); send_all(b, n); }
    free(c);
}

static void handle_line(char *line) {
    cmd_t *c = calloc(1, sizeof *c);
    long a = 0;  /* %li: peek addresses go up to 0xFFFFFFFF */
    if (sscanf(line, !strncmp(line, "peek", 4) ? "%11s %li %i" : "%11s %ld %d", c->verb, &a, &c->b) < 1) { free(c); return; }
    c->a = (int)(uint32_t)a;
    post(apply, c);
}

static void *srv_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd p[NCLI + 2];
        int map[NCLI + 2], np = 0;
        p[np].fd = lfd, p[np].events = POLLIN, map[np++] = -1;
        p[np].fd = wake[0], p[np].events = POLLIN, map[np++] = -2;
        pthread_mutex_lock(&mu);
        for (int i = 0; i < NCLI; i++)
            if (cli[i].fd >= 0) p[np].fd = cli[i].fd, p[np].events = POLLIN | (cli[i].n ? POLLOUT : 0), map[np++] = i;
        pthread_mutex_unlock(&mu);
        if (poll(p, np, -1) < 0 && errno != EINTR) break;
        for (int k = 0; k < np; k++) {
            if (!p[k].revents) continue;
            if (map[k] == -2) { char b[64]; if (read(wake[0], b, sizeof b) < 0) {} continue; }
            if (map[k] == -1) {
                int fd = accept(lfd, 0, 0);
                if (fd < 0) continue;
                pthread_mutex_lock(&mu);
                int i = 0;
                while (i < NCLI && cli[i].fd >= 0) i++;
                if (i == NCLI) { close(fd); pthread_mutex_unlock(&mu); continue; }
                cli[i].fd = fd, cli[i].n = 0, cli[i].inn = 0;
                const char *h = "hello mpk3emu 1\n";
                memcpy(cli[i].out, h, strlen(h)), cli[i].n = strlen(h);
                memcpy(cli[i].out + cli[i].n, cur_oled, cur_oled_n), cli[i].n += cur_oled_n;
                memcpy(cli[i].out + cli[i].n, cur_leds, cur_leds_n), cli[i].n += cur_leds_n;
                memcpy(cli[i].out + cli[i].n, cur_status, cur_status_n), cli[i].n += cur_status_n;
                pthread_mutex_unlock(&mu);
                continue;
            }
            int i = map[k];
            if (p[k].revents & POLLOUT) {
                pthread_mutex_lock(&mu);
                ssize_t w = write(cli[i].fd, cli[i].out, cli[i].n);
                if (w > 0) memmove(cli[i].out, cli[i].out + w, cli[i].n - w), cli[i].n -= w;
                pthread_mutex_unlock(&mu);
            }
            if (p[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r = read(cli[i].fd, cli[i].in + cli[i].inn, sizeof cli[i].in - 1 - cli[i].inn);
                if (r <= 0) {
                    pthread_mutex_lock(&mu);
                    close(cli[i].fd), cli[i].fd = -1, cli[i].n = 0;
                    pthread_mutex_unlock(&mu);
                    continue;
                }
                cli[i].inn += r;
                char *s = cli[i].in, *e;
                while ((e = memchr(s, '\n', cli[i].in + cli[i].inn - s))) { *e = 0; handle_line(s); s = e + 1; }
                cli[i].inn -= s - cli[i].in;
                memmove(cli[i].in, s, cli[i].inn);
                if (cli[i].inn == sizeof cli[i].in - 1) cli[i].inn = 0;  /* overlong line */
            }
        }
    }
    return 0;
}

void panel_srv_init(const char *path) {
    for (int i = 0; i < NCLI; i++) cli[i].fd = -1, cli[i].out = malloc(OUTMAX);
    if (pipe(wake) < 0) { perror("pipe"); exit(1); }
    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof a.sun_path) { LOG("socket path too long (max %zu): %s", sizeof a.sun_path - 1, path); exit(1); }
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    unlink(path);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 4) < 0) { perror(path); exit(1); }
    pthread_t t;
    pthread_create(&t, 0, srv_thread, 0);
    LOG("panel socket %s", path);
}
