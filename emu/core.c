/* CPU + system core: Unicorn Cortex-M4, memory map, MMIO dispatch, events, exceptions (entry/return/priorities),
 * NVIC, SCB, SysTick, DWT, reset, real-time pacing. Unicorn has the M-profile CPU but no NVIC: exception entry is
 * done here between blocks, and exception return arrives as EXCP_EXCEPTION_EXIT (8) through UC_HOOK_INTR. */
#include "core.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unicorn/arm.h>

uc_engine *uc;
uint8_t *flash_mem;
uint8_t sram[SRAM_SIZE] __attribute__((aligned(4096)));
int emu_verbose;
uint8_t *sysmem;  /* 0x1FFFF000 page (flash size, UID, user system data = option bytes), stored after the flash */

/* ---- time ---- */
static uint64_t cycles;      /* counted at block entry */
static uint64_t stop_at = ~0ull;
static volatile int stop_req;
uint64_t now(void) { return cycles; }
void stall(uint64_t c) { cycles += c; }

/* ---- events ---- */
#define NEV 64
static struct { uint64_t when; ev_fn fn; void *ctx; } ev[NEV];
static int nev;

static void ev_recalc(void) {
    uint64_t m = ~0ull;
    for (int i = 0; i < nev; i++) if (ev[i].when < m) m = ev[i].when;
    stop_at = m;
}
void ev_cancel(ev_fn fn, void *ctx) {
    for (int i = 0; i < nev; i++)
        if (ev[i].fn == fn && ev[i].ctx == ctx) { ev[i] = ev[--nev]; break; }
    ev_recalc();
}
void ev_at(uint64_t when, ev_fn fn, void *ctx) {
    ev_cancel(fn, ctx);
    if (nev == NEV) { LOG("event table full"); abort(); }
    ev[nev].when = when, ev[nev].fn = fn, ev[nev].ctx = ctx, nev++;
    if (when < stop_at) stop_at = when;
}
static void ev_run(void) {
    for (;;) {
        int b = -1;
        for (int i = 0; i < nev; i++) if (ev[i].when <= cycles && (b < 0 || ev[i].when < ev[b].when)) b = i;
        if (b < 0) break;
        ev_fn fn = ev[b].fn;
        void *ctx = ev[b].ctx;
        ev[b] = ev[--nev];
        fn(ctx);
    }
    ev_recalc();
}

/* ---- cross-thread posts ---- */
static pthread_mutex_t post_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { void (*fn)(void *); void *ctx; } posts[256];
static int nposts;
void post(void (*fn)(void *), void *ctx) {
    pthread_mutex_lock(&post_mu);
    while (nposts == 256) { pthread_mutex_unlock(&post_mu); nanosleep(&(struct timespec){0, 1000000}, 0); pthread_mutex_lock(&post_mu); }
    posts[nposts].fn = fn, posts[nposts].ctx = ctx, nposts++;
    stop_req = 1;
    pthread_mutex_unlock(&post_mu);
}
static void run_posts(void) {
    pthread_mutex_lock(&post_mu);
    int n = nposts;
    typeof(posts[0]) p[256];
    memcpy(p, posts, n * sizeof p[0]);
    nposts = 0;
    pthread_mutex_unlock(&post_mu);
    for (int i = 0; i < n; i++) p[i].fn(p[i].ctx);
}

/* ---- reset hooks ---- */
static struct { void (*fn)(void *); void *ctx; } resets[32];
static int nresets;
static const char *reset_why;
void on_reset(void (*fn)(void *), void *ctx) { resets[nresets].fn = fn, resets[nresets].ctx = ctx, nresets++; }
void request_reset(const char *why) { reset_why = why; stop_req = 1; uc_emu_stop(uc); }

/* ---- MMIO dispatch ---- */
#define NMMIO 64
static struct { const char *name; uint32_t base, size; rd_fn rd; wr_fn wr; void *ctx; } mm[NMMIO];
static int nmm;
void mmio_add(const char *name, uint32_t base, uint32_t size, rd_fn rd, wr_fn wr, void *ctx) {
    mm[nmm].name = name, mm[nmm].base = base, mm[nmm].size = size, mm[nmm].rd = rd, mm[nmm].wr = wr, mm[nmm].ctx = ctx;
    nmm++;
}

#define NLOGGED 4096
static uint32_t logged[NLOGGED];
static int nlogged;
void mmio_unmodelled(const char *what, uint32_t addr, int is_write, uint32_t val) {
    uint32_t key = addr | (uint32_t)is_write;  /* addresses of registers are at least 2-aligned in practice */
    if (!emu_verbose) {
        for (int i = 0; i < nlogged; i++) if (logged[i] == key) return;
        if (nlogged < NLOGGED) logged[nlogged++] = key;
    }
    uint32_t pc;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    if (is_write) LOG("unmodelled %s write 0x%08x = 0x%x (pc 0x%08x)", what, addr, val, pc);
    else LOG("unmodelled %s read 0x%08x (pc 0x%08x)", what, addr, pc);
}

/* plain storage behind the unregistered parts of each window */
static uint8_t store_periph[0x30000], store_usb[0x40000], store_ppb[0x100000];

static uint32_t st_rd(uint8_t *s, uint32_t off, int size) {
    uint32_t v = 0;
    memcpy(&v, s + off, size);
    return v;
}
static void st_wr(uint8_t *s, uint32_t off, uint32_t v, int size) { memcpy(s + off, &v, size); }

static inline void fix_apply(void);
static uint64_t win_rd(uint32_t base, uint8_t *store, uint64_t off, unsigned size) {
    uint32_t a = base + (uint32_t)off;
    fix_apply();
    for (int i = 0; i < nmm; i++)
        if (a - mm[i].base < mm[i].size) return mm[i].rd ? mm[i].rd(mm[i].ctx, a - mm[i].base, size) : 0;
    mmio_unmodelled("register", a, 0, 0);
    return st_rd(store, off, size);
}
static void win_wr(uint32_t base, uint8_t *store, uint64_t off, unsigned size, uint64_t v) {
    uint32_t a = base + (uint32_t)off;
    fix_apply();
    for (int i = 0; i < nmm; i++)
        if (a - mm[i].base < mm[i].size) { if (mm[i].wr) mm[i].wr(mm[i].ctx, a - mm[i].base, (uint32_t)v, size); return; }
    mmio_unmodelled("register", a, 1, (uint32_t)v);
    st_wr(store, off, (uint32_t)v, size);
}
static uint64_t periph_rd(uc_engine *u, uint64_t off, unsigned size, void *d) { (void)u, (void)d; return win_rd(0x40000000, store_periph, off, size); }
static void periph_wr(uc_engine *u, uint64_t off, unsigned size, uint64_t v, void *d) { (void)u, (void)d; win_wr(0x40000000, store_periph, off, size, v); }
static uint64_t usb_rd(uc_engine *u, uint64_t off, unsigned size, void *d) { (void)u, (void)d; return win_rd(0x50000000, store_usb, off, size); }
static void usb_wr(uc_engine *u, uint64_t off, unsigned size, uint64_t v, void *d) { (void)u, (void)d; win_wr(0x50000000, store_usb, off, size, v); }
static uint64_t ppb_rd(uc_engine *u, uint64_t off, unsigned size, void *d) { (void)u, (void)d; return win_rd(0xE0000000, store_ppb, off, size); }
static void ppb_wr(uc_engine *u, uint64_t off, unsigned size, uint64_t v, void *d) { (void)u, (void)d; win_wr(0xE0000000, store_ppb, off, size, v); }

/* bit-band: alias word n -> bit (n % 32) of word at target + 4 * (n / 32) */
static uint32_t bb_target_rd(uint32_t a) {
    uint32_t v = 0;
    if (a >= 0x40000000) return periph_rd(uc, a - 0x40000000, 4, 0);
    uc_mem_read(uc, a, &v, 4);
    return v;
}
static void bb_target_wr(uint32_t a, uint32_t v) {
    if (a >= 0x40000000) periph_wr(uc, a - 0x40000000, 4, v, 0);
    else uc_mem_write(uc, a, &v, 4);
}
static uint64_t bb_rd(uc_engine *u, uint64_t off, unsigned size, void *d) {
    (void)u, (void)size;
    uint32_t t = (uint32_t)(uintptr_t)d + (uint32_t)(off >> 5 << 2);
    return bb_target_rd(t) >> (off >> 2 & 31) & 1;
}
static void bb_wr(uc_engine *u, uint64_t off, unsigned size, uint64_t v, void *d) {
    (void)u, (void)size;
    uint32_t t = (uint32_t)(uintptr_t)d + (uint32_t)(off >> 5 << 2), bit = 1u << (off >> 2 & 31);
    uint32_t w = bb_target_rd(t);
    bb_target_wr(t, (v & 1) ? w | bit : w & ~bit);
}

/* bus master access for DMA (core.h): peripherals through their models, memory through Unicorn */
uint32_t bus_read(uint32_t a, int size) {
    uint32_t v = 0;
    if (a - 0x40000000 < 0x30000) return (uint32_t)periph_rd(uc, a - 0x40000000, size, 0);
    uc_mem_read(uc, a, &v, size);
    return v;
}
void bus_write(uint32_t a, uint32_t v, int size) {
    if (a - 0x40000000 < 0x30000) periph_wr(uc, a - 0x40000000, size, v, 0);
    else uc_mem_write(uc, a, &v, size);
}

/* ---- flash writes: flash is mapped read/execute; the flash controller (sysctl.c) gets every CPU store ---- */
extern int flash_store(uint32_t addr, int size, uint32_t val);  /* sysctl.c; returns 0 = bus error */
static int flash_bus_error;
/* Unicorn still performs a store to a read-only mem_map_ptr page after this hook returns true, so the bytes the
 * flash controller decided on are put back at the next sync point (block start, MMIO access, stop). */
static struct { uint8_t *p; uint8_t v[8]; int n; } fix[8];
static int nfix;
static inline void fix_apply(void) {
    for (int i = 0; i < nfix; i++) memcpy(fix[i].p, fix[i].v, fix[i].n);
    nfix = 0;
}
static bool hook_prot(uc_engine *u, uc_mem_type t, uint64_t addr, int size, int64_t val, void *d) {
    (void)u, (void)d;
    uint8_t *p = 0;
    if (addr - 0x1FFFF000 < 0x1000) p = sysmem + (addr - 0x1FFFF000);
    else if (addr - FLASH_BASE < FLASH_SIZE) p = flash_mem + (addr - FLASH_BASE);
    else if (addr < FLASH_SIZE) p = flash_mem + addr;
    if (t != UC_MEM_WRITE_PROT || !p || size > 8) return false;
    uint32_t a = addr - 0x1FFFF000 < 0x1000 ? (uint32_t)addr : (uint32_t)(addr % FLASH_SIZE) + FLASH_BASE;
    if (addr - 0x1FFFF000 < 0x1000 && addr - 0x1FFFF800 >= 16) return false;  /* UID etc.: bus error */
    if (!flash_store(a, size, (uint32_t)val)) { flash_bus_error = 1; return false; }
    if (nfix == 8) fix_apply();
    fix[nfix].p = p, fix[nfix].n = size;
    memcpy(fix[nfix].v, p, size);
    nfix++;
    return true;
}

/* ---- NVIC / exceptions ---- */
#define NIRQ 96
#define NEXC (16 + NIRQ)
static uint8_t pend[NEXC], active[NEXC], enabled[NEXC], level[NIRQ], prio[NEXC];
static uint32_t vtor, aircr_prigroup, ccr = 0x200, shcsr, cfsr, hfsr, bfar, scr;
static int pend_dirty;
static uint64_t exc_count[NEXC];
void core_stats(void) {
    for (int n = 0; n < NEXC; n++) if (exc_count[n]) LOG("exception %d taken %llu times", n, (unsigned long long)exc_count[n]);
}  /* something may be deliverable: check in the block hook */

static int exc_prio(int n) {
    if (n == 2) return -2;
    if (n == 3) return -1;
    int mask = 0xFF & ~((2 << aircr_prigroup) - 1);  /* group priority bits */
    return prio[n] & mask;
}
static int exc_enabled(int n) {
    if (n >= 16) return enabled[n];
    if (n == 4) return shcsr >> 16 & 1;
    if (n == 5) return shcsr >> 17 & 1;
    if (n == 6) return shcsr >> 18 & 1;
    return 1;
}
static int exec_prio(void) {
    int p = 256;
    for (int n = 2; n < NEXC; n++) if (active[n] && exc_prio(n) < p) p = exc_prio(n);
    uint32_t primask = 0, basepri = 0;
    uc_reg_read(uc, UC_ARM_REG_PRIMASK, &primask);
    uc_reg_read(uc, UC_ARM_REG_BASEPRI, &basepri);
    if (basepri & 0xFF) {
        int b = basepri & 0xFF & ~((2 << aircr_prigroup) - 1);
        if (b < p) p = b;
    }
    if (primask & 1 && p > 0) p = 0;
    return p;
}
static int best_pending(void) {  /* highest-priority pending + enabled exception, or 0 */
    int best = 0, bp = 1 << 20;
    for (int n = 2; n < NEXC; n++)
        if (pend[n] && exc_enabled(n) && exc_prio(n) < bp) best = n, bp = exc_prio(n);
    return best;
}
static int deliverable(void) {
    int n = best_pending();
    return n && (n == 2 || exc_prio(n) < exec_prio()) ? n : 0;
}
static void set_pend(int n) { pend[n] = 1; pend_dirty = 1; }
void irq_pulse(int irq) { if (irq < NIRQ) set_pend(16 + irq); }
void irq_level(int irq, int lvl) {
    if (irq >= NIRQ) return;
    level[irq] = lvl != 0;
    if (lvl && !active[16 + irq]) set_pend(16 + irq);
}

static uint32_t rd32(uint32_t a) { uint32_t v = 0; uc_mem_read(uc, a, &v, 4); return v; }

static void exc_entry(int n) {
    uint32_t r[8], sp, xpsr, ipsr;
    static const int regs[6] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12, UC_ARM_REG_LR};
    for (int i = 0; i < 6; i++) uc_reg_read(uc, regs[i], &r[i]);
    uc_reg_read(uc, UC_ARM_REG_PC, &r[6]);
    uc_reg_read(uc, UC_ARM_REG_XPSR, &xpsr);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    ipsr = xpsr & 0x1FF;
    uint32_t align = (sp >> 2 & 1) & (ccr >> 9 & 1);
    sp = (sp - 32) & ~(align << 2);
    r[7] = (xpsr & ~(1u << 9)) | align << 9;
    if (uc_mem_write(uc, sp, r, 32) != UC_ERR_OK) { LOG("exception %d: stack 0x%08x not writable, reset", n, sp); request_reset("lockup"); return; }
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uint32_t lr = ipsr ? 0xFFFFFFF1 : 0xFFFFFFF9;
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uint32_t nx = (xpsr & 0xF8000000u) | (1u << 24) | (uint32_t)n;  /* flags kept, IT cleared, T set */
    uc_reg_write(uc, UC_ARM_REG_XPSR, &nx);
    uint32_t pc = rd32(vtor + 4 * n) & ~1u;
    uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    pend[n] = 0, active[n] = 1;
    exc_count[n]++;
    if (emu_verbose > 1) LOG("exception %d -> 0x%08x", n, pc);
}

static void exc_return(uint32_t exc_ret) {
    uint32_t xpsr, sp, r[8];
    uc_reg_read(uc, UC_ARM_REG_XPSR, &xpsr);
    int n = xpsr & 0x1FF;
    if (exc_ret & 4) LOG("exception return to PSP is not modelled (EXC_RETURN 0x%08x)", exc_ret | 1);
    active[n] = 0;
    if (n >= 16 && level[n - 16]) set_pend(n);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_mem_read(uc, sp, r, 32);
    static const int regs[6] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12, UC_ARM_REG_LR};
    for (int i = 0; i < 6; i++) uc_reg_write(uc, regs[i], &r[i]);
    sp += 32 + (r[7] >> 9 & 1) * 4;
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uint32_t x = r[7] & ~(1u << 9);
    uc_reg_write(uc, UC_ARM_REG_XPSR, &x);
    uint32_t pc = r[6] & ~1u;
    uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    pend_dirty = 1;
}

/* a fault from the running code: escalate to HardFault when the handler is off or can't preempt */
static void fault(int n, uint32_t cfsr_bits, const char *what) {
    uint32_t pc;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    LOG("%s at pc 0x%08x", what, pc);
    cfsr |= cfsr_bits;
    if (n != 3 && (!exc_enabled(n) || exc_prio(n) >= exec_prio())) n = 3, hfsr |= 1u << 30;
    if (n == 3 && exec_prio() <= -1) { LOG("fault in HardFault/NMI: lockup, reset"); request_reset("lockup"); return; }
    exc_entry(n);
}

/* ---- SysTick ---- */
static uint32_t st_csr, st_rvr;
static uint64_t st_origin;  /* counter was 0 at this cycle (and wraps every RVR+1 ticks) */
static uint32_t st_div(void) { return st_csr & 4 ? 1 : 8; }
static uint32_t st_cvr(void) {
    if (!(st_csr & 1)) return 0;
    uint64_t period = (uint64_t)st_rvr + 1, m = (cycles - st_origin) / st_div() % period;
    return m ? (uint32_t)(period - m) : 0;
}
static void st_fire(void *ctx);
static void st_arm(void) {
    ev_cancel(st_fire, 0);
    if (!(st_csr & 1) || !st_rvr) return;
    uint64_t period = ((uint64_t)st_rvr + 1) * st_div(), k = (cycles - st_origin) / period + 1;
    ev_at(st_origin + k * period, st_fire, 0);
}
static void st_fire(void *ctx) {
    (void)ctx;
    st_csr |= 1u << 16;
    if (st_csr & 2) set_pend(15);
    st_arm();
}

/* ---- DWT ---- */
static uint32_t dwt_ctrl = 0x40000000, demcr;
static uint64_t dwt_base;
static uint32_t dwt_frozen;

/* ---- PPB registers ---- */
static uint32_t ppb_read(void *ctx, uint32_t off, int size) {
    uint32_t a = (uint32_t)(uintptr_t)ctx + off;
    off = a - 0xE0000000;
    if (a >= 0xE000E100 && a < 0xE000E400) {  /* ISER/ICER/ISPR/ICPR/IABR: 32 IRQs per word */
        int w = (a & 0x7F) / 4, kind = (a - 0xE000E100) / 0x80;
        uint32_t v = 0;
        for (int b = 0; b < 32 && 32 * w + b < NIRQ; b++) {
            int n = 16 + 32 * w + b;
            int bit = kind <= 1 ? enabled[n] : kind <= 3 ? pend[n] : kind == 4 ? active[n] : 0;
            v |= (uint32_t)bit << b;
        }
        return v;
    }
    if (a >= 0xE000E400 && a < 0xE000E400 + NIRQ) {
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)prio[16 + (a - 0xE000E400) + i] << (8 * i);
        return v;
    }
    if (a >= 0xE000ED18 && a < 0xE000ED24) {
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)prio[4 + (a - 0xE000ED18) + i] << (8 * i);
        return v;
    }
    switch (a & ~3u) {
    case 0xE000E004: return 2;  /* ICTR: 96 interrupt lines */
    case 0xE000E010: { uint32_t v = st_csr; st_csr &= ~(1u << 16); return v; }
    case 0xE000E014: return st_rvr;
    case 0xE000E018: return st_cvr();
    case 0xE000E01C: return 0x40000000 | (F_CPU / 8 / 100 - 1);  /* CALIB: no reference clock, 10 ms */
    case 0xE000ED00: return 0x410FC241;  /* CPUID: Cortex-M4 r0p1 */
    case 0xE000ED04: {
        int act = 0, np = best_pending();
        uint32_t xpsr;
        uc_reg_read(uc, UC_ARM_REG_XPSR, &xpsr);
        act = xpsr & 0x1FF;
        uint32_t v = (uint32_t)act | (uint32_t)np << 12 | (np >= 16) << 22;
        if (pend[15]) v |= 1u << 26;
        if (pend[14]) v |= 1u << 28;
        if (pend[2]) v |= 1u << 31;
        return v;
    }
    case 0xE000ED08: return vtor;
    case 0xE000ED0C: return 0xFA050000 | aircr_prigroup << 8;
    case 0xE000ED10: return scr;
    case 0xE000ED14: return ccr;
    case 0xE000ED24: {
        uint32_t v = shcsr & 0x70000;
        static const int bit[] = {[4] = 0, [5] = 1, [6] = 3, [11] = 7, [12] = 8, [14] = 10, [15] = 11};
        for (int n = 4; n <= 15; n++) if ((n <= 6 || n == 11 || n == 12 || n == 14 || n == 15) && active[n]) v |= 1u << bit[n];
        return v;
    }
    case 0xE000ED28: return cfsr;
    case 0xE000ED2C: return hfsr;
    case 0xE000ED38: return bfar;
    case 0xE000EDFC: return demcr;
    case 0xE0001000: return dwt_ctrl;
    case 0xE0001004: return dwt_ctrl & 1 ? (uint32_t)(cycles - dwt_base) : dwt_frozen;
    }
    mmio_unmodelled("PPB", a, 0, 0);
    return st_rd(store_ppb, off, size);
}

static void ppb_write(void *ctx, uint32_t off, uint32_t v, int size) {
    uint32_t a = (uint32_t)(uintptr_t)ctx + off;
    off = a - 0xE0000000;
    if (a >= 0xE000E100 && a < 0xE000E300) {
        int w = (a & 0x7F) / 4, kind = (a - 0xE000E100) / 0x80;
        for (int b = 0; b < 32 && 32 * w + b < NIRQ; b++) {
            if (!(v >> b & 1)) continue;
            int n = 16 + 32 * w + b;
            if (kind == 0) enabled[n] = 1;
            else if (kind == 1) enabled[n] = 0;
            else if (kind == 2) pend[n] = 1;
            else if (level[n - 16] && !active[n]) pend[n] = 1;  /* ICPR: a high level line re-pends */
            else pend[n] = 0;
        }
        pend_dirty = 1;
        return;
    }
    if (a >= 0xE000E400 && a < 0xE000E400 + NIRQ) {
        for (int i = 0; i < size; i++) prio[16 + (a - 0xE000E400) + i] = (v >> (8 * i)) & 0xF0;  /* 4 bits */
        pend_dirty = 1;
        return;
    }
    if (a >= 0xE000ED18 && a < 0xE000ED24) {
        for (int i = 0; i < size; i++) prio[4 + (a - 0xE000ED18) + i] = (v >> (8 * i)) & 0xF0;
        pend_dirty = 1;
        return;
    }
    switch (a & ~3u) {
    case 0xE000E010: st_csr = (st_csr & (1u << 16)) | (v & 7); st_arm(); return;
    case 0xE000E014: st_rvr = v & 0xFFFFFF; st_arm(); return;
    case 0xE000E018: st_origin = cycles; st_csr &= ~(1u << 16); st_arm(); return;
    case 0xE000ED04:
        if (v & (1u << 31)) set_pend(2);
        if (v & (1u << 28)) set_pend(14);
        if (v & (1u << 27)) pend[14] = 0;
        if (v & (1u << 26)) set_pend(15);
        if (v & (1u << 25)) pend[15] = 0;
        return;
    case 0xE000ED08: vtor = v & 0xFFFFFF80; return;
    case 0xE000ED0C:
        if ((v >> 16) != 0x05FA) return;
        aircr_prigroup = v >> 8 & 7;
        if (v & 5) request_reset("AIRCR");
        pend_dirty = 1;
        return;
    case 0xE000ED10: scr = v; return;
    case 0xE000ED14: ccr = v; return;
    case 0xE000ED24: shcsr = v; pend_dirty = 1; return;
    case 0xE000ED28: cfsr &= ~v; return;
    case 0xE000ED2C: hfsr &= ~v; return;
    case 0xE000EDFC: demcr = v; return;
    case 0xE0001000:
        if ((v & 1) && !(dwt_ctrl & 1)) dwt_base = cycles - dwt_frozen;
        if (!(v & 1) && (dwt_ctrl & 1)) dwt_frozen = (uint32_t)(cycles - dwt_base);
        dwt_ctrl = (dwt_ctrl & 0xFFFF0000) | (v & 0xFFFF);
        return;
    case 0xE0001004: dwt_base = cycles - v, dwt_frozen = v; return;
    case 0xE000EF00: set_pend(16 + (v & 0x1FF)); return;  /* STIR */
    }
    mmio_unmodelled("PPB", a, 1, v);
    st_wr(store_ppb, off, v, size);
}

/* ---- CPU hooks ---- */
static int intr_no = -1;
static int count_insns(uint32_t addr, uint32_t size) {
    const uint8_t *p;
    if (addr - FLASH_BASE < FLASH_SIZE) p = flash_mem + (addr - FLASH_BASE);
    else if (addr < FLASH_SIZE) p = flash_mem + addr;
    else if (addr - SRAM_BASE < SRAM_SIZE) p = sram + (addr - SRAM_BASE);
    else return (int)(size + 2) / 3;
    int n = 0;
    for (uint32_t i = 0; i + 1 < size; n++) {
        uint16_t h = p[i] | p[i + 1] << 8;
        i += (h >> 11) >= 0x1D ? 4 : 2;
    }
    return n ? n : 1;
}
int emu_profile;
#define NPROF 4096
static struct { uint32_t pc; uint64_t n; } prof[NPROF];
static uint64_t prof_next;
static void prof_hit(uint32_t pc) {
    unsigned h = (pc >> 1) % NPROF;
    for (unsigned i = 0; i < NPROF; i++, h = (h + 1) % NPROF)
        if (prof[h].pc == pc || !prof[h].n) { prof[h].pc = pc, prof[h].n++; return; }
}
static int prof_cmp(const void *a, const void *b) {
    uint64_t x = ((const typeof(prof[0]) *)a)->n, y = ((const typeof(prof[0]) *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}
void core_profile_dump(void) {
    if (!emu_profile) return;
    qsort(prof, NPROF, sizeof prof[0], prof_cmp);
    uint64_t tot = 0;
    for (int i = 0; i < NPROF; i++) tot += prof[i].n;
    for (int i = 0; i < 25 && prof[i].n; i++) LOG("profile %5.1f%% block 0x%08x", 100.0 * prof[i].n / tot, prof[i].pc);
}
static void hook_block(uc_engine *u, uint64_t addr, uint32_t size, void *d) {
    (void)d;
    if (nfix) fix_apply();
    if (emu_profile && ++prof_next % 64 == 0) prof_hit((uint32_t)addr);
    cycles += count_insns((uint32_t)addr, size);
    if (cycles >= stop_at || stop_req) { uc_emu_stop(u); return; }
    if (pend_dirty) {
        if (deliverable()) uc_emu_stop(u);
        else pend_dirty = best_pending() != 0;  /* keep checking while something waits for the mask to drop */
    }
}
static void hook_intr(uc_engine *u, uint32_t intno, void *d) {
    (void)d;
    intr_no = (int)intno;
    uc_emu_stop(u);
}

/* ---- reset ---- */
static void chip_reset(void) {
    memset(pend, 0, sizeof pend), memset(active, 0, sizeof active), memset(enabled, 0, sizeof enabled);
    memset(level, 0, sizeof level), memset(prio, 0, sizeof prio);
    vtor = 0, aircr_prigroup = 0, ccr = 0x200, shcsr = cfsr = hfsr = bfar = scr = 0;
    st_csr = st_rvr = 0;
    dwt_ctrl = 0x40000000, demcr = 0, dwt_base = cycles, dwt_frozen = 0;
    for (int i = 0; i < nev; i++) ev[i].fn = 0;
    nev = 0;
    ev_recalc();
    memset(store_periph, 0, sizeof store_periph), memset(store_usb, 0, sizeof store_usb);
    for (int i = 0; i < nresets; i++) resets[i].fn(resets[i].ctx);
    uc_ctl_flush_tb(uc);
    uint32_t zero = 0, x = 1u << 24, sp, pc;
    static const int regs[] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
                               UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
                               UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_PRIMASK,
                               UC_ARM_REG_BASEPRI, UC_ARM_REG_CONTROL};
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) uc_reg_write(uc, regs[i], &zero);
    uint32_t lr = 0xFFFFFFFF;
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uc_reg_write(uc, UC_ARM_REG_XPSR, &x);
    memcpy(&sp, flash_mem, 4), memcpy(&pc, flash_mem + 4, 4);  /* boot from flash (BOOT0 low) */
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    pc &= ~1u;
    uc_reg_write(uc, UC_ARM_REG_PC, &pc);
}

/* ---- pacing: never run ahead of the wall clock ---- */
static double wall0;
static uint64_t cyc0;
int emu_fast;  /* --fast: no pacing (tests) */
static double wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void pace(void *ctx) {
    (void)ctx;
    double ahead = (double)(cycles - cyc0) / F_CPU - (wall() - wall0);
    if (ahead > 0.002 && !emu_fast) {
        nanosleep(&(struct timespec){0, (long)(ahead * 1e9)}, 0);
    } else if (ahead < -0.2) {  /* fell behind (slow host, debugger): don't try to catch up in a burst */
        wall0 = wall(), cyc0 = cycles;
    }
    ev_at(cycles + F_CPU / 1000, pace, 0);
}
static void pace_reset(void *ctx) { (void)ctx; wall0 = wall(), cyc0 = cycles; ev_at(cycles + F_CPU / 1000, pace, 0); }

/* ---- setup + run loop ---- */
static void check(uc_err e, const char *what) {
    if (e != UC_ERR_OK) { LOG("%s: %s", what, uc_strerror(e)); exit(1); }
}

static uint32_t ppb_rd_reg(void *ctx, uint32_t off, int size) { return ppb_read(ctx, off, size); }

void core_sysmem_defaults(uint8_t *page) {  /* a new flash file: factory state of the 0x1FFFF000 page */
    memset(page, 0xFF, 0x1000);
    page[0x7E0] = 128, page[0x7E1] = 0;  /* F_SIZE: 128 KB */
    page[0x7E2] = page[0x7E3] = 0;       /* the stock bootloader reads F_SIZE as a word (erase/program range check) */
    static const uint8_t uid[12] = {0x4D, 0x50, 0x4B, 0x33, 0x45, 0x4D, 0x55, 0x00, 0x12, 0x34, 0x56, 0x78};
    memcpy(page + 0x7E8, uid, 12);
    static const uint8_t usd[16] = {0xA5, 0x5A, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    memcpy(page + 0x800, usd, 16);
}

void core_init(uint8_t *flash) {
    flash_mem = flash;
    check(uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, &uc), "uc_open");
    check(uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M4), "cpu model");
    check(uc_mem_map_ptr(uc, 0x00000000, FLASH_SIZE, UC_PROT_READ | UC_PROT_EXEC, flash_mem), "map flash alias");
    check(uc_mem_map_ptr(uc, FLASH_BASE, FLASH_SIZE, UC_PROT_READ | UC_PROT_EXEC, flash_mem), "map flash");
    check(uc_mem_map_ptr(uc, SRAM_BASE, SRAM_SIZE, UC_PROT_ALL, sram), "map sram");
    sysmem = flash + FLASH_SIZE;
    check(uc_mem_map_ptr(uc, 0x1FFFF000, 0x1000, UC_PROT_READ, sysmem), "map system memory");
    check(uc_mmio_map(uc, 0x40000000, 0x30000, periph_rd, 0, periph_wr, 0), "map peripherals");
    check(uc_mmio_map(uc, 0x50000000, 0x40000, usb_rd, 0, usb_wr, 0), "map usb");
    check(uc_mmio_map(uc, 0xE0000000, 0x100000, ppb_rd, 0, ppb_wr, 0), "map ppb");
    check(uc_mmio_map(uc, 0x22000000, SRAM_SIZE * 32, bb_rd, (void *)(uintptr_t)SRAM_BASE, bb_wr, (void *)(uintptr_t)SRAM_BASE), "map sram bit-band");
    check(uc_mmio_map(uc, 0x42000000, 0x30000 * 32, bb_rd, (void *)(uintptr_t)0x40000000, bb_wr, (void *)(uintptr_t)0x40000000), "map periph bit-band");
    mmio_add("SysTick/NVIC/SCB", 0xE000E000, 0x1000, ppb_rd_reg, ppb_write, (void *)0xE000E000);
    mmio_add("DWT", 0xE0001000, 0x1000, ppb_rd_reg, ppb_write, (void *)0xE0001000);
    uc_hook h;
    check(uc_hook_add(uc, &h, UC_HOOK_BLOCK, (void *)hook_block, 0, 1, 0), "block hook");
    check(uc_hook_add(uc, &h, UC_HOOK_INTR, (void *)hook_intr, 0, 1, 0), "intr hook");
    check(uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE_PROT, (void *)hook_prot, 0, 1, 0), "prot hook");
    on_reset(pace_reset, 0);
}

uint32_t core_pc(void) { uint32_t pc; uc_reg_read(uc, UC_ARM_REG_PC, &pc); return pc; }

/* runs until *quit becomes nonzero (any thread; call core_kick() after setting it) */
void core_kick(void) { stop_req = 1; uc_emu_stop(uc); }

void core_run(volatile int *quit) {
    reset_why = "power-up";
    while (!*quit) {
        if (reset_why) {
            LOG("reset (%s)", reset_why);
            if (!strncmp(reset_why, "power", 5)) memset(sram, 0, sizeof sram);  /* SRAM doesn't survive power loss */
            reset_why = 0;
            chip_reset();
        }
        stop_req = 0;
        run_posts();
        ev_run();
        int n = deliverable();
        if (n) exc_entry(n);
        pend_dirty = best_pending() != 0;
        if (reset_why) continue;
        if (stop_req) continue;
        uint32_t pc = core_pc();
        intr_no = -1;
        flash_bus_error = 0;
        uc_err e = uc_emu_start(uc, pc | 1, 1, 0, 0);
        fix_apply();
        if (intr_no == 8) {
            exc_return(core_pc());
        } else if (intr_no >= 0) {
            uint32_t p = core_pc();
            if (intr_no == 1) fault(6, 1u << 16, "undefined instruction");  /* UsageFault UNDEFINSTR */
            else if (intr_no == 2) set_pend(11);                         /* SVC: PC already past it */
            else if (intr_no == 7) { LOG("BKPT at 0x%08x, skipped", p); p += 2; uc_reg_write(uc, UC_ARM_REG_PC, &p); }
            else fault(6, 1u << 17, "CPU exception");                    /* INVSTATE and friends */
            if (intr_no != 1 && intr_no != 2 && intr_no != 7) LOG("(unicorn exception %d)", intr_no);
        } else if (e != UC_ERR_OK) {
            if (e == UC_ERR_FETCH_UNMAPPED || e == UC_ERR_FETCH_PROT) fault(4, 1u << 0, "instruction fetch from bad address");
            else if (e == UC_ERR_READ_UNMAPPED || e == UC_ERR_WRITE_UNMAPPED || flash_bus_error ||
                     e == UC_ERR_WRITE_PROT || e == UC_ERR_READ_PROT) fault(5, 1u << 9, "bus error (data access)");
            else if (e == UC_ERR_INSN_INVALID) fault(6, 1u << 16, "invalid instruction");
            else { LOG("unicorn: %s at 0x%08x", uc_strerror(e), core_pc()); fault(3, 0, "emulation error"); }
        }
    }
}
