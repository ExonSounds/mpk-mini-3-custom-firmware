/* RCC (clocks report ready at once), PWC/IOMUX as plain registers, and the flash controller (STM32F1-style:
 * unlock keys, sector erase, halfword program) on the mmap'ed flash file. AT32F415RB: 128 sectors of 1 KB. */
#include "core.h"
#include <string.h>

/* ---- plain registers: storage + nothing else (writes logged with -v only) ---- */
typedef struct { uint8_t r[0x400]; } plain_t;
static uint32_t plain_rd(void *ctx, uint32_t off, int size) { uint32_t v = 0; memcpy(&v, ((plain_t *)ctx)->r + off, size); return v; }
static void plain_wr(void *ctx, uint32_t off, uint32_t v, int size) { memcpy(((plain_t *)ctx)->r + off, &v, size); }
static void plain_reset(void *ctx) { memset(ctx, 0, sizeof(plain_t)); }
static plain_t pwc, iomux, exint;

/* ---- RCC ---- */
static uint32_t rcc[0x100];
static uint32_t rcc_rd(void *ctx, uint32_t off, int size) {
    (void)ctx, (void)size;
    uint32_t v = rcc[off / 4];
    if (off == 0x00) {  /* CR: HSI/HSE/PLL ready = on */
        v = (v & ~((1u << 1) | (1u << 17) | (1u << 25))) | (v & 1) << 1 | (v >> 16 & 1) << 17 | (v >> 24 & 1) << 25;
    } else if (off == 0x04) {  /* CFGR: SWS = SW */
        v = (v & ~0xCu) | (v & 3) << 2;
    } else if (off == 0x24) {  /* CSR: LSI ready = on, reset flags: POR */
        v = (v & ~2u) | (v & 1) << 1;
    } else if (off == 0x20) {  /* BDCR: LSE ready = on */
        v = (v & ~2u) | (v & 1) << 1;
    }
    return v >> (8 * (off & 3));
}
static void rcc_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    (void)ctx;
    uint32_t *r = &rcc[off / 4];
    if (size == 4) *r = v;
    else {
        uint32_t sh = 8 * (off & 3), m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
        *r = (*r & ~m) | (v << sh & m);
    }
}
static void rcc_reset(void *ctx) {
    (void)ctx;
    memset(rcc, 0, sizeof rcc);
    rcc[0] = 0x83;          /* HSI on */
    rcc[0x24 / 4] = 0x0C000000;  /* PORRST | PINRST */
}

/* ---- flash controller ---- */
#define KEY1 0x45670123u
#define KEY2 0xCDEF89ABu
static uint32_t f_acr, f_sr, f_cr, f_ar, f_keyst, f_optst;
#define SECTOR 1024u
#define ERASE_US 6000.0    /* shortcut: typical sector erase time is a guess (AT32 datasheet range ~2-10 ms) */
#define PROG_US 42.0

static void flash_touched(uint32_t a, uint32_t n) { uc_ctl_remove_cache(uc, a, a + n); uc_ctl_remove_cache(uc, a - FLASH_BASE, a - FLASH_BASE + n); }
/* uc_ctl_remove_cache inside Unicorn's write-protect hook crashes Unicorn: programmed halfwords drop their cached
 * code at the next stop instead (post() from the emulator thread stops the CPU after the current block) */
static uint32_t touched_lo = ~0u, touched_hi;
static void touched_flush(void *ctx) {
    (void)ctx;
    flash_touched(touched_lo, touched_hi - touched_lo);
    touched_lo = ~0u, touched_hi = 0;
}

static uint32_t obr(void) {  /* option bytes as loaded at reset */
    return (sysmem[0x800] != 0xA5) << 1 | (uint32_t)sysmem[0x802] << 2;
}
static uint32_t f_obr;

int flash_store(uint32_t addr, int size, uint32_t val) {  /* CPU store into flash (core.c) */
    if (addr - 0x1FFFF800 < 16) {  /* user system data: OPTPG with the option keys given */
        if (!(f_cr & (1u << 4)) || !(f_cr & (1u << 9)) || size == 1) { f_sr |= 1u << 2; return 1; }
        uint32_t o = 0x800 + (addr & 0xE);
        LOG("option byte write 0x%08x = 0x%04x", addr, val & 0xFFFF);
        if (sysmem[o] != 0xFF) { f_sr |= 1u << 2; return 1; }
        sysmem[o] = val & 0xFF, sysmem[o + 1] = ~val & 0xFF;  /* the complement byte is generated */
        stall(us_to_cycles(PROG_US));
        f_sr |= 1u << 5;
        return 1;
    }
    if (!(f_cr & 1) || (f_cr & 0x80)) {  /* PG off or locked: the write does nothing */
        if (emu_verbose) LOG("flash write without PG at 0x%08x", addr);
        f_sr |= 1u << 2;  /* PRGERR */
        return 1;
    }
    if (size == 1) return 0;
    for (int h = 0; h < size / 2; h++) {
        uint32_t o = addr - FLASH_BASE + 2 * h;
        uint16_t old = flash_mem[o] | flash_mem[o + 1] << 8, nv = (uint16_t)(val >> (16 * h));
        if (old != 0xFFFF && nv != 0) { f_sr |= 1u << 2; continue; }  /* not erased: PRGERR, no write */
        flash_mem[o] = nv & 0xFF, flash_mem[o + 1] = nv >> 8;
        stall(us_to_cycles(PROG_US));
    }
    if (touched_lo == ~0u) post(touched_flush, 0);
    if (addr < touched_lo) touched_lo = addr;
    if (addr + size > touched_hi) touched_hi = addr + size;
    f_sr |= 1u << 5;  /* EOP */
    return 1;
}

static uint32_t flash_rd(void *ctx, uint32_t off, int size) {
    (void)ctx, (void)size;
    switch (off) {
    case 0x00: return f_acr;
    case 0x0C: return f_sr;
    case 0x10: return f_cr;
    case 0x14: return f_ar;
    case 0x1C: return f_obr;
    case 0x20: return 0xFFFFFFFF;  /* no write protection */
    }
    mmio_unmodelled("FLASH", 0x40022000 + off, 0, 0);
    return 0;
}
static void flash_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    (void)ctx, (void)size;
    switch (off) {
    case 0x00: f_acr = v; return;
    case 0x08:  /* option byte keys */
        if (f_optst == 0 && v == KEY1) f_optst = 1;
        else if (f_optst == 1 && v == KEY2 && !(f_cr & 0x80)) f_optst = 0, f_cr |= 1u << 9;
        else f_optst = 0;
        return;
    case 0x04:
        if (f_keyst == 0 && v == KEY1) f_keyst = 1;
        else if (f_keyst == 1 && v == KEY2) f_keyst = 0, f_cr &= ~0x80u;
        else f_keyst = 0, f_cr |= 0x80;  /* wrong key: locked until reset */
        return;
    case 0x0C: f_sr &= ~(v & 0x34); return;
    case 0x14: f_ar = v; return;
    case 0x10:
        if (f_cr & 0x80) return;
        f_cr = (v & 0x14F7) | (f_cr & v & (1u << 9));  /* OPTWRE: only cleared by software */
        if (v & (1u << 6)) {  /* STRT */
            f_cr &= ~(1u << 6);
            if ((f_cr & (1u << 5)) && (f_cr & (1u << 9))) {  /* option byte erase */
                memset(sysmem + 0x800, 0xFF, 16);
                LOG("option bytes erased");
                stall(us_to_cycles(ERASE_US));
            } else if (f_cr & 4) {  /* mass erase */
                memset(flash_mem, 0xFF, FLASH_SIZE);
                flash_touched(FLASH_BASE, FLASH_SIZE);
                stall(us_to_cycles(ERASE_US * 8));
            } else if (f_cr & 2) {
                uint32_t a = (f_ar - FLASH_BASE) & ~(SECTOR - 1);
                if (a < FLASH_SIZE) {
                    memset(flash_mem + a, 0xFF, SECTOR);
                    flash_touched(FLASH_BASE + a, SECTOR);
                    stall(us_to_cycles(ERASE_US));
                } else f_sr |= 1u << 4;
            }
            f_sr |= 1u << 5;
        }
        return;
    }
    mmio_unmodelled("FLASH", 0x40022000 + off, 1, v);
}
static void flash_reset(void *ctx) { (void)ctx; f_acr = 0x30, f_sr = 0, f_cr = 0x80, f_ar = 0, f_keyst = f_optst = 0, f_obr = obr(); }

void sysctl_init(void) {
    mmio_add("RCC", 0x40021000, 0x400, rcc_rd, rcc_wr, 0);
    on_reset(rcc_reset, 0);
    mmio_add("FLASH", 0x40022000, 0x400, flash_rd, flash_wr, 0);
    on_reset(flash_reset, 0);
    mmio_add("PWC", 0x40007000, 0x400, plain_rd, plain_wr, &pwc);
    on_reset(plain_reset, &pwc);
    mmio_add("IOMUX", 0x40010000, 0x400, plain_rd, plain_wr, &iomux);
    on_reset(plain_reset, &iomux);
    mmio_add("EXINT", 0x40010400, 0x400, plain_rd, plain_wr, &exint);
    on_reset(plain_reset, &exint);
}
