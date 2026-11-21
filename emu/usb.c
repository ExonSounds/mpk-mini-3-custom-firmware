/* OTGFS at 0x50000000: Synopsys DWC2 OTG core, device mode, as the custom firmware (fw/usb_midi.c) and the stock
 * firmware (ST USB-OTG library) drive it. Slave mode (no DMA), full speed only.
 *
 * Modelled: GRSTCTL soft reset/flushes (instant), GINTSTS latched + computed bits (RXFLVL, GINAKEFF, GONAKEFF,
 * IEPINT, OEPINT), the level IRQ 67 (GAHBCFG.GINT && GINTSTS & GINTMSK), the RX FIFO (GRXSTSR/GRXSTSP + data pops,
 * space from GRXFSIZ; popping "setup done" / "transfer complete" entries raises STUP / XFRC as on the chip), one
 * TX FIFO per IN endpoint (depth from DIEPTXFn, DTXFSTS, TXFE by GAHBCFG.TXFELVL, DIEPEMPMSK), DIEPCTL/DOEPCTL
 * (EPENA/EPDIS set-only, SNAK/CNAK -> NAKSTS, STALL, cleared on EP0 by SETUP), DxEPTSIZ packet/size counting,
 * DCTL soft disconnect + global IN/OUT NAK, DCFG, DSTS frame number and speed.
 * Not modelled: data toggles (the host never sees errors), suspend/resume, isochronous, host mode, DMA.
 * shortcut: TX FIFO n belongs to IN endpoint n (both firmwares set TXFNUM = endpoint), upgrade if one doesn't. */
#include "core.h"
#include "usb.h"
#include <string.h>

#define NEP 4
#define EPENA (1u << 31)
#define EPDIS (1u << 30)
#define SNAK (1u << 27)
#define CNAK (1u << 26)
#define STALL (1u << 21)
#define NAKSTS (1u << 17)
#define XFRC 1u
#define EPDISD 2u
#define STUP 8u
#define TXFE 0x80u
/* GINTSTS bits the core latches (cleared by writing 1): MMIS SOF ESUSP USBSUSP USBRST ENUMDNE ISOODRP EOPF
 * IISOIXFR IPXFR CIDSCHG DISCINT SRQINT WKUPINT */
#define GI_LATCHED 0xF030FC0Au
#define GI_SOF (1u << 3)
#define GI_RXFLVL (1u << 4)
#define GI_USBRST (1u << 12)
#define GI_ENUMDNE (1u << 13)

static uint32_t gotgctl, gahbcfg, gusbcfg, grstctl, gintsts, gintmsk, grxfsiz, gccfg, cid, txf[NEP];
static uint32_t dcfg, dctl, diepmsk, doepmsk, daintmsk, diepempmsk, pcgcctl;
static int ginak, gonak, pullup;
static unsigned frame;
static struct { uint32_t ctl, intr, tsiz, nak; } in[NEP], out[NEP];

/* TX FIFOs: words written by the CPU, waiting for IN tokens */
static struct { uint32_t w[256]; int head, n; } tx[NEP];
static int tx_depth(int ep) {
    int d = (ep ? txf[ep] : txf[0]) >> 16;
    return d > 256 || d == 0 ? 256 : d;
}

/* RX FIFO: entries of (status word, data), popped by GRXSTSP and then FIFO reads */
#define NRX 32
static struct { uint32_t sts; uint8_t d[64]; } rx[NRX], cur;
static int rx_head, rx_n, rx_words, cur_pos;
static int rx_depth(void) { int d = grxfsiz & 0xFFFF; return d ? d : 128; }
static int rx_push(int ep, int pktsts, const uint8_t *d, int len) {
    int words = 1 + (len + 3) / 4;
    if (rx_n == NRX || rx_words + words > rx_depth()) return 0;
    int i = (rx_head + rx_n++) % NRX;
    rx[i].sts = (uint32_t)ep | (uint32_t)len << 4 | (uint32_t)pktsts << 17 | (frame & 15) << 21;
    if (len) memcpy(rx[i].d, d, len);
    rx_words += words;
    return 1;
}
static int rx_free(void) { return NRX - rx_n >= 2 ? rx_depth() - rx_words : 0; }

/* ---- interrupt state ---- */
static uint32_t diepint_eff(int n) {  /* TXFE is a live status bit: the endpoint's FIFO is (half) empty */
    int fr = tx_depth(n) - tx[n].n;
    int empty = gahbcfg & 0x80 ? fr == tx_depth(n) : 2 * fr >= tx_depth(n);
    return in[n].intr | (empty ? TXFE : 0);
}
static uint32_t daint(void) {
    uint32_t v = 0;
    for (int n = 0; n < NEP; n++) {
        if (diepint_eff(n) & (diepmsk | (diepempmsk >> n & 1) << 7)) v |= 1u << n;
        if (out[n].intr & doepmsk) v |= 1u << (16 + n);
    }
    return v;
}
static uint32_t gintsts_eff(void) {
    uint32_t d = daint() & daintmsk;
    return gintsts | (rx_n ? GI_RXFLVL : 0) | 1u << 5 | ginak << 6 | gonak << 7 | (d & 0xFFFF ? 1u << 18 : 0) |
           (d >> 16 ? 1u << 19 : 0);
}
static void update(void) {
    irq_level(67, (gahbcfg & 1) && (gintsts_eff() & gintmsk));
    int p = (gccfg & (1u << 16)) && !(dctl & 2);  /* PHY powered and not soft-disconnected: D+ pull-up on */
    if (p != pullup) pullup = p, usbhost_connect(p);
}

static void flush_tx(int n) {
    for (int i = 0; i < NEP; i++) if (n == 0x10 || n == i) tx[i].n = tx[i].head = 0;
}
static void flush_rx(void) { rx_n = rx_words = rx_head = 0, cur_pos = 64; cur.sts = 0; }

static void soft_reset(void) {  /* GRSTCTL.CSRST: state machines, FIFOs and interrupts; most CSRs keep values */
    flush_tx(0x10), flush_rx();
    gintsts = 0, ginak = gonak = 0;
    for (int n = 0; n < NEP; n++) in[n].intr = out[n].intr = 0;
}

static void chip_reset(void *ctx) {
    (void)ctx;
    gotgctl = gahbcfg = gintsts = gintmsk = gccfg = dctl = diepmsk = doepmsk = daintmsk = diepempmsk = pcgcctl = 0;
    gusbcfg = 0xA00, grxfsiz = 0x200, cid = 0x1200, dcfg = 0x02200000, frame = 0;
    for (int n = 0; n < NEP; n++) txf[n] = 0x02000200, in[n].ctl = out[n].ctl = in[n].tsiz = out[n].tsiz = 0;
    for (int n = 0; n < NEP; n++) in[n].nak = out[n].nak = 1;
    soft_reset();
    pullup = 0;
    usbhost_connect(0);
    irq_level(67, 0);
}

/* ---- endpoint registers ---- */
static uint32_t ctl_rd(int n, int is_in) {
    uint32_t c = (is_in ? in : out)[n].ctl | ((is_in ? in : out)[n].nak ? NAKSTS : 0);
    return n ? c : c | 1u << 15;  /* EP0 is always active */
}
static void ctl_wr(int n, int is_in, uint32_t v) {
    typeof(&in[0]) e = &(is_in ? in : out)[n];
    uint32_t keep = e->ctl & EPENA;  /* EPENA can only be set by software and cleared by the core */
    e->ctl = keep | (v & ~(EPENA | EPDIS | SNAK | CNAK | (3u << 28)));
    if (v & EPENA) e->ctl |= EPENA;
    if (v & SNAK) e->nak = 1;
    if (v & CNAK) e->nak = 0;
    if ((v & EPDIS) && (e->ctl & EPENA)) e->ctl &= ~EPENA, e->intr |= EPDISD, e->nak = 1;
}

static uint32_t usb_rd(void *ctx, uint32_t off, int size) {
    (void)ctx, (void)size;
    uint32_t v = 0;
    if (off >= 0x1000 && off < 0x1000 + 0x1000 * 16) {  /* any FIFO address reads the RX FIFO */
        for (int i = 0; i < 4; i++, cur_pos++) if (cur_pos < 64) v |= (uint32_t)cur.d[cur_pos] << (8 * i);
        return v;
    }
    int n = (off >> 5) & (NEP - 1);  /* NEP is a power of 2 */
    if (off >= 0x900 && off < 0x900 + 0x20 * NEP) switch (off & 0x1F) {
        case 0x00: return ctl_rd(n, 1);
        case 0x08: return diepint_eff(n);
        case 0x10: return in[n].tsiz;
        case 0x18: return (uint32_t)(tx_depth(n) - tx[n].n);  /* DTXFSTS: free words */
        }
    if (off >= 0xB00 && off < 0xB00 + 0x20 * NEP) switch (off & 0x1F) {
        case 0x00: return ctl_rd(n, 0);
        case 0x08: return out[n].intr;
        case 0x10: return out[n].tsiz;
        }
    if (off >= 0x104 && off < 0x100 + 4 * NEP) return txf[(off - 0x100) / 4];
    switch (off) {
    case 0x000: return gotgctl | 0xD0000;  /* CIDSTS = B-device, A/B session valid */
    case 0x008: return gahbcfg;
    case 0x00C: return gusbcfg;
    case 0x010: return grstctl | 1u << 31;  /* AHB idle; resets and flushes finish at once */
    case 0x014: return gintsts_eff();
    case 0x018: return gintmsk;
    case 0x01C:
    case 0x020:
        if (!rx_n) return 0;
        v = rx[rx_head].sts;
        if (off == 0x020) {
            cur = rx[rx_head], cur_pos = 0;
            rx_head = (rx_head + 1) % NRX, rx_n--, rx_words -= 1 + (int)((v >> 4 & 0x7FF) + 3) / 4;
            int ep = v & 15, st = v >> 17 & 15;
            if (st == 3) out[ep].intr |= XFRC;  /* "transfer complete" entry popped */
            if (st == 4) out[0].intr |= STUP;   /* "setup done" entry popped */
            update();
        }
        return v;
    case 0x024: return grxfsiz;
    case 0x028: return txf[0];
    case 0x02C: return 8u << 16 | (uint32_t)(tx_depth(0) - tx[0].n);  /* GNPTXSTS */
    case 0x038: return gccfg;
    case 0x03C: return cid;
    case 0x800: return dcfg;
    case 0x804: return dctl | ginak << 2 | gonak << 3;
    case 0x808: return (frame & 0x3FFF) << 8 | 3u << 1;  /* DSTS: full speed (48 MHz PHY clock) */
    case 0x810: return diepmsk;
    case 0x814: return doepmsk;
    case 0x818: return daint();
    case 0x81C: return daintmsk;
    case 0x834: return diepempmsk;
    case 0xE00: return pcgcctl;
    }
    mmio_unmodelled("OTGFS", 0x50000000 + off, 0, 0);
    return 0;
}

static void usb_wr(void *ctx, uint32_t off, uint32_t v, int size) {
    (void)ctx, (void)size;
    if (off >= 0x1000 && off < 0x1000 + 0x1000 * NEP) {  /* push one word into IN endpoint n's TX FIFO */
        int n = (off - 0x1000) >> 12;
        if (tx[n].n < tx_depth(n)) tx[n].w[(tx[n].head + tx[n].n++) % 256] = v;
        else LOG("OTGFS: TX FIFO %d overflow", n);
        update();
        return;
    }
    int n = (off >> 5) & (NEP - 1), done = 1;
    if (off >= 0x900 && off < 0x900 + 0x20 * NEP) switch (off & 0x1F) {
        case 0x00: ctl_wr(n, 1, v); break;
        case 0x08: in[n].intr &= ~v; break;
        case 0x10: in[n].tsiz = v; break;
        default: done = 0;
        }
    else if (off >= 0xB00 && off < 0xB00 + 0x20 * NEP) switch (off & 0x1F) {
        case 0x00: ctl_wr(n, 0, v); break;
        case 0x08: out[n].intr &= ~v; break;
        case 0x10: out[n].tsiz = v; break;
        default: done = 0;
        }
    else if (off >= 0x104 && off < 0x100 + 4 * NEP) txf[(off - 0x100) / 4] = v;
    else switch (off) {
    case 0x000: gotgctl = v; break;
    case 0x004: break;  /* GOTGINT: no OTG events */
    case 0x008: gahbcfg = v; break;
    case 0x00C: gusbcfg = v; break;
    case 0x010:
        if (v & 1) soft_reset();
        if (v & 0x10) flush_rx();
        if (v & 0x20) flush_tx(v >> 6 & 0x1F);
        grstctl = v & (0x1F << 6);
        break;
    case 0x014: gintsts &= ~(v & GI_LATCHED); break;
    case 0x018: gintmsk = v; break;
    case 0x024: grxfsiz = v; break;
    case 0x028: txf[0] = v; break;
    case 0x038: gccfg = v; break;
    case 0x03C: cid = v; break;
    case 0x800: dcfg = v; break;
    case 0x804:
        if (v & 1u << 7) ginak = 1;
        if (v & 1u << 8) ginak = 0;
        if (v & 1u << 9) gonak = 1;
        if (v & 1u << 10) gonak = 0;
        dctl = v & ~0xF8Cu;
        break;
    case 0x810: diepmsk = v; break;
    case 0x814: doepmsk = v; break;
    case 0x818: break;  /* DAINT is read-only */
    case 0x81C: daintmsk = v; break;
    case 0x834: diepempmsk = v; break;
    case 0xE00: pcgcctl = v; break;
    default: done = 0;
    }
    if (!done) mmio_unmodelled("OTGFS", 0x50000000 + off, 1, v);
    update();
}

/* ---- the wire, as seen from the host ---- */
static int ep_mps(int n, uint32_t ctl) { return n ? (int)(ctl & 0x7FF) : 64 >> (ctl & 3); }

int usb_dev_setup(const uint8_t s[8]) {
    if (rx_free() < 4) return USB_NAK;  /* really no handshake; the host retries either way */
    rx_push(0, 6, s, 8);
    rx_push(0, 4, 0, 0);
    in[0].ctl &= ~STALL, out[0].ctl &= ~STALL, out[0].nak = 1;
    update();
    return USB_ACK;
}

int usb_dev_in(int ep, uint8_t *buf, int *len) {
    *len = 0;
    if (ep >= NEP) return USB_STALL;
    typeof(&in[0]) e = &in[ep];
    if (e->ctl & STALL) return USB_STALL;
    if (!(e->ctl & EPENA) || e->nak || ginak) return USB_NAK;
    uint32_t xmask = ep ? 0x7FFFF : 0x7F, pmask = ep ? 0x3FF : 3;
    int xfr = e->tsiz & xmask, pkt = e->tsiz >> 19 & pmask, mps = ep_mps(ep, e->ctl);
    int n = xfr < mps ? xfr : mps, words = (n + 3) / 4;
    if (!pkt || tx[ep].n < words) return USB_NAK;  /* FIFO underrun: the core NAKs */
    for (int i = 0; i < words; i++) {
        uint32_t w = tx[ep].w[tx[ep].head];
        tx[ep].head = (tx[ep].head + 1) % 256, tx[ep].n--;
        for (int j = 0; j < 4 && 4 * i + j < n; j++) buf[4 * i + j] = w >> (8 * j);
    }
    *len = n, xfr -= n, pkt--;
    e->tsiz = (e->tsiz & ~(xmask | pmask << 19)) | (uint32_t)xfr | (uint32_t)pkt << 19;
    if (!pkt) e->ctl &= ~EPENA, e->intr |= XFRC;
    update();
    return USB_ACK;
}

int usb_dev_out(int ep, const uint8_t *buf, int len) {
    if (ep >= NEP) return USB_STALL;
    typeof(&out[0]) e = &out[ep];
    if (e->ctl & STALL) return USB_STALL;
    if (!(e->ctl & EPENA) || e->nak || gonak || rx_free() < 2 + (len + 3) / 4) return USB_NAK;
    uint32_t xmask = ep ? 0x7FFFF : 0x7F, pmask = ep ? 0x3FF : 1;
    int xfr = e->tsiz & xmask, pkt = e->tsiz >> 19 & pmask, mps = ep_mps(ep, e->ctl);
    rx_push(ep, 2, buf, len);
    xfr = xfr > len ? xfr - len : 0, pkt = pkt ? pkt - 1 : 0;
    e->tsiz = (e->tsiz & ~(xmask | pmask << 19)) | (uint32_t)xfr | (uint32_t)pkt << 19;
    if (!pkt || len < mps) rx_push(ep, 3, 0, 0), e->ctl &= ~EPENA, e->nak = 1;  /* XFRC once that entry is popped */
    update();
    return USB_ACK;
}

void usb_dev_bus_reset(int start) {
    gintsts |= start ? GI_USBRST : GI_ENUMDNE;
    update();
}

void usb_dev_sof(unsigned f) {
    frame = f;
    gintsts |= GI_SOF;
    update();
}

void usb_init(void) {
    mmio_add("OTGFS", 0x50000000, 0x40000, usb_rd, usb_wr, 0);
    on_reset(chip_reset, 0);
}
