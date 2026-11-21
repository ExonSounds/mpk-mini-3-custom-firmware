/* mpk3emu: the emulator core process. Usage: see usage() below; mpk3_emu.py (repo root) is the launcher. */
#include "core.h"
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void core_init(uint8_t *flash);
void core_run(volatile int *quit);
void core_kick(void);
uint32_t core_pc(void);
void core_stats(void);
void core_sysmem_defaults(uint8_t *page);
extern int emu_fast, emu_profile;
void core_profile_dump(void);
void sysctl_init(void);
void gpio_init(void);
#include "periph.h"
void usb_init(void);
void midi_bridge_init(void);

static volatile int quit;
static void on_signal(int s) { (void)s; quit = 1; core_kick(); }
void core_quit(void) { quit = 1; core_kick(); }
void panel_srv_init(const char *path);
const char *emu_run_dir;

static double stop_after;
static void stop_ev(void *ctx) { (void)ctx; LOG("stopping after %.3f s, pc 0x%08x", stop_after, core_pc()); quit = 1; core_kick(); }
static void arm_stop(void *ctx) { (void)ctx; if (stop_after > 0) ev_at(now() + (uint64_t)(stop_after * F_CPU), stop_ev, 0); }

static void usage(void) {
    fprintf(stderr,
            "usage: mpk3emu FLASHFILE [--image IMAGE] [--fast] [--seconds S] [--run-dir DIR] [--profile] [-v]\n"
            "  FLASHFILE  flash state: 128 KB flash + the 4 KB system page (option bytes); created from --image\n"
            "  --image    write this 128 KB image into FLASHFILE first (like flashing the keyboard)\n"
            "  --fast     don't pace to real time\n"
            "  --seconds  stop after S seconds of emulated time\n"
            "  --run-dir  directory for the panel socket and MIDI links (default $XDG_RUNTIME_DIR/mpk3-emu)\n"
            "  -v         log every unmodelled register access (-vv: exceptions too)\n");
    exit(2);
}

static uint8_t *open_flash(const char *path, const char *image) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror(path); exit(1); }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size == FLASH_SIZE || (size == 0 && image)) {  /* new state file (or a plain image): add the system page */
        uint8_t page[0x1000];
        core_sysmem_defaults(page);
        if (pwrite(fd, page, sizeof page, FLASH_SIZE) != (ssize_t)sizeof page) { perror(path); exit(1); }
    }
    if (image) {
        FILE *f = fopen(image, "rb");
        if (!f) { perror(image); exit(1); }
        uint8_t buf[FLASH_SIZE];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (n != FLASH_SIZE) { fprintf(stderr, "%s: %zu bytes, expected %u\n", image, n, FLASH_SIZE); exit(1); }
        if (pwrite(fd, buf, sizeof buf, 0) != (ssize_t)sizeof buf) { perror(path); exit(1); }
    }
    if (lseek(fd, 0, SEEK_END) != STATE_SIZE) { fprintf(stderr, "%s: not a flash state file (use --image)\n", path); exit(1); }
    uint8_t *m = mmap(0, STATE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    return m;
}

int main(int argc, char **argv) {
    const char *flash = 0, *image = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(argv[i], "--fast")) emu_fast = 1;
        else if (!strcmp(argv[i], "--run-dir") && i + 1 < argc) emu_run_dir = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) stop_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--profile")) emu_profile = 1;
        else if (!strcmp(argv[i], "-v")) emu_verbose = 1;
        else if (!strcmp(argv[i], "-vv")) emu_verbose = 2;
        else if (argv[i][0] != '-' && !flash) flash = argv[i];
        else usage();
    }
    if (!flash) usage();
    uint8_t *mem = open_flash(flash, image);
    if (!emu_run_dir) {
        static char d[512];
        const char *x = getenv("XDG_RUNTIME_DIR");
        snprintf(d, sizeof d, "%s/mpk3-emu", x ? x : "/tmp");
        emu_run_dir = d;
    }
    mkdir(emu_run_dir, 0700);
    char sock[600];
    snprintf(sock, sizeof sock, "%s/panel.sock", emu_run_dir);

    core_init(mem);
    sysctl_init();
    gpio_init();
    inputs_init(), dma_init(), adc1_init(), spi_init(), tmr_init(), oled_init(), leds_init();
    usb_init();
    midi_bridge_init();
    on_reset(arm_stop, 0);
    panel_srv_init(sock);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    core_run(&quit);
    core_stats();
    core_profile_dump();
    msync(mem, STATE_SIZE, MS_SYNC);
    return 0;
}
