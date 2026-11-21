/* Core setup values follow the stock firmware (ST USB-OTG library, CoreInit at 0x080159CA): on-chip PHY,
 * GCCFG = 0x002D0000 (power up + VBUS sensing, bit 21 = ignore VBUS on AT32), full speed, 320-word FIFO RAM. */
#include "usb_midi.h"
#include "board.h"

#define OTG(o)       R32(0x50000000u + (o))
#define GAHBCFG      OTG(0x008)
#define GUSBCFG      OTG(0x00C)
#define GRSTCTL      OTG(0x010)
#define GINTSTS      OTG(0x014)
#define GINTMSK      OTG(0x018)
#define GRXSTSP      OTG(0x020)
#define GRXFSIZ      OTG(0x024)
#define DIEPTXF0     OTG(0x028)
#define GCCFG        OTG(0x038)
#define DIEPTXF1     OTG(0x104)
#define DCFG         OTG(0x800)
#define DCTL         OTG(0x804)
#define DIEPMSK      OTG(0x810)
#define DOEPMSK      OTG(0x814)
#define DAINT        OTG(0x818)
#define DAINTMSK     OTG(0x81C)
#define DIEPCTL(n)   OTG(0x900 + 0x20 * (n))
#define DIEPINT(n)   OTG(0x908 + 0x20 * (n))
#define DIEPTSIZ(n)  OTG(0x910 + 0x20 * (n))
#define DOEPCTL(n)   OTG(0xB00 + 0x20 * (n))
#define DOEPINT(n)   OTG(0xB08 + 0x20 * (n))
#define DOEPTSIZ(n)  OTG(0xB10 + 0x20 * (n))
#define PCGCCTL      OTG(0xE00)
#define FIFO(n)      OTG(0x1000 + 0x1000 * (n))

#define EPENA  (1u << 31)
#define CNAK   (1u << 26)
#define SNAK   (1u << 27)
#define STALL  (1u << 21)
#define USBAEP (1u << 15)
#define SD0PID (1u << 28)
#define BULK   (2u << 18)
#define GI_RXFLVL (1u << 4)
#define GI_USBRST (1u << 12)
#define GI_ENUMDNE (1u << 13)
#define GI_IEPINT (1u << 18)
#define GI_OEPINT (1u << 19)

/* pid.codes test VID/PID: a private test device, not Akai's 09e8:1049 (update mode keeps Akai's IDs). */
static const uint8_t dev_desc[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 0, 1};

static const uint8_t cfg_desc[101] = {
    9, 2, 101, 0, 2, 1, 0, 0x80, 250,         /* config: 2 interfaces, bus powered, 500 mA */
    9, 4, 0, 0, 0, 1, 1, 0, 0,                /* interface 0: Audio Control */
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 1,       /* AC header, streaming interface 1 */
    9, 4, 1, 0, 2, 1, 3, 0, 0,                /* interface 1: MIDI Streaming, 2 endpoints */
    7, 0x24, 1, 0x00, 0x01, 65, 0,            /* MS header, class-specific length 65 */
    6, 0x24, 2, 1, 1, 0,                      /* MIDI IN jack, embedded, id 1 (host -> us) */
    6, 0x24, 2, 2, 2, 0,                      /* MIDI IN jack, external, id 2 */
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,             /* MIDI OUT jack, embedded, id 3 (us -> host), from jack 2 */
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,             /* MIDI OUT jack, external, id 4, from jack 1 */
    9, 5, 0x01, 2, 64, 0, 0, 0, 0,            /* EP1 OUT bulk 64 */
    5, 0x25, 1, 1, 1,                         /*   -> embedded IN jack 1 */
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,            /* EP1 IN bulk 64 */
    5, 0x25, 1, 1, 3,                         /*   <- embedded OUT jack 3 */
};

static const char *const strings[] = {0, "DIY", "MPK mini 3 custom"};

static volatile int configured;
static uint32_t setup[2];
static uint8_t strbuf[64];

static void fifo_write(int ep, const uint8_t *p, int n) {
    for (int i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (int j = 0; j < 4 && i + j < n; j++) w |= (uint32_t)p[i + j] << (8 * j);
        FIFO(ep) = w;
    }
}

static void ep0_out_arm(void) {
    DOEPTSIZ(0) = (3u << 29) | (1u << 19) | 64;  /* accept up to 3 SETUPs or one 64-byte OUT (status stage) */
    DOEPCTL(0) |= EPENA | CNAK;
}

static void ep0_in(const void *data, int len) {
    /* shortcut: no ZLP after a transfer that is an exact multiple of 64 and shorter than wLength; none of our
     * descriptors hits that case. EP0 XFRSIZ is 7 bits, so len must stay <= 127 (the config descriptor is 101). */
    int pk = len ? (len + 63) / 64 : 1;
    DIEPTSIZ(0) = ((uint32_t)pk << 19) | (uint32_t)len;
    DIEPCTL(0) |= EPENA | CNAK;
    fifo_write(0, data, len);
}

static void ep0_stall(void) {
    DIEPCTL(0) |= STALL;
    DOEPCTL(0) |= STALL;  /* the core clears EP0 STALL when the next SETUP arrives */
}

static void flush_tx(int n) {  /* n = FIFO number, 0x10 = all */
    GRSTCTL = (1u << 5) | ((uint32_t)n << 6);
    for (int t = 0; t < 100000 && (GRSTCTL & (1u << 5)); t++) {}
}

static void configure_ep1(void) {
    DIEPCTL(1) = USBAEP | BULK | (1u << 22) | 64 | SD0PID | SNAK;  /* TX FIFO 1 */
    DOEPCTL(1) = USBAEP | BULK | 64 | SD0PID;
    flush_tx(1);
    DOEPTSIZ(1) = (1u << 19) | 64;
    DOEPCTL(1) |= EPENA | CNAK;
    DAINTMSK |= (1u << 1) | (1u << 17);
}

static void handle_setup(void) {
    const uint8_t *s = (const uint8_t *)setup;
    uint16_t value = s[2] | s[3] << 8, length = s[6] | s[7] << 8;
    static const uint8_t zero2[2] = {0, 0};
    uint8_t one;
    const uint8_t *d = 0;
    int n = -1;  /* -1 = stall */

    if ((s[0] & 0x60) == 0) switch (s[1]) {  /* standard requests only; MIDI needs no class requests */
    case 6:  /* GET_DESCRIPTOR */
        if (value >> 8 == 1) d = dev_desc, n = sizeof dev_desc;
        else if (value >> 8 == 2) d = cfg_desc, n = sizeof cfg_desc;
        else if (value >> 8 == 3 && (value & 0xFF) == 0) {
            static const uint8_t langid[4] = {4, 3, 0x09, 0x04};
            d = langid, n = 4;
        } else if (value >> 8 == 3 && (value & 0xFF) < 3) {
            const char *str = strings[value & 0xFF];
            int len = 0;
            while (str[len] && len < 31) { strbuf[2 + 2 * len] = str[len]; strbuf[3 + 2 * len] = 0; len++; }
            strbuf[0] = 2 + 2 * len, strbuf[1] = 3;
            d = strbuf, n = strbuf[0];
        }  /* device qualifier etc.: stall (full-speed only device) */
        break;
    case 5:  /* SET_ADDRESS: DWC2 takes the address now, before the status stage */
        DCFG = (DCFG & ~(0x7Fu << 4)) | ((value & 0x7Fu) << 4);
        n = 0;
        break;
    case 9:  /* SET_CONFIGURATION */
        if (value == 1) configure_ep1();
        configured = value == 1;
        n = 0;
        break;
    case 8: one = configured; d = &one, n = 1; break;     /* GET_CONFIGURATION */
    case 0: d = zero2, n = 2; break;                       /* GET_STATUS */
    case 10: d = zero2, n = 1; break;                      /* GET_INTERFACE: alt 0 */
    case 1: case 3: case 11: n = 0; break;                 /* CLEAR/SET_FEATURE, SET_INTERFACE: accept */
    }

    if (n < 0) ep0_stall();
    else ep0_in(d, n < length ? n : length);
    ep0_out_arm();
}

void OTGFS_IRQHandler(void) {
    uint32_t st = GINTSTS & GINTMSK;

    if (st & GI_USBRST) {
        DCTL &= ~1u;
        flush_tx(0x10);
        for (int i = 0; i < 4; i++) { DIEPINT(i) = 0xFF; DOEPINT(i) = 0xFF; DOEPCTL(i) |= SNAK; }
        DAINTMSK = (1u << 0) | (1u << 16);
        DOEPMSK = (1u << 3) | 1u;  /* SETUP done, transfer complete */
        DIEPMSK = 1u;
        DCFG &= ~(0x7Fu << 4);
        configured = 0;
        ep0_out_arm();
        GINTSTS = GI_USBRST;
    }
    if (st & GI_ENUMDNE) {
        DIEPCTL(0) &= ~3u;  /* EP0 max packet 64 */
        DCTL |= 1u << 8;    /* clear global IN NAK */
        GINTSTS = GI_ENUMDNE;
    }
    while (GINTSTS & GI_RXFLVL) {
        uint32_t rx = GRXSTSP;
        int ep = rx & 0xF, bcnt = (rx >> 4) & 0x7FF, pktsts = (rx >> 17) & 0xF;
        if (pktsts == 6) {  /* SETUP packet */
            setup[0] = FIFO(0);
            setup[1] = FIFO(0);
        } else if (pktsts == 2) {  /* OUT data: status ZLPs on EP0, USB-MIDI packets from the host on EP1 */
            for (int i = 0; i < bcnt; i += 4) {
                uint32_t w = FIFO(0);
                if (ep == 1) usb_midi_received(w);
            }
        }
    }
    if (st & GI_OEPINT) {
        uint32_t daint = DAINT;
        if (daint & (1u << 16)) {
            uint32_t i = DOEPINT(0);
            DOEPINT(0) = i;
            if (i & (1u << 3)) handle_setup();
            else if (i & 1u) ep0_out_arm();
        }
        if (daint & (1u << 17)) {
            uint32_t i = DOEPINT(1);
            DOEPINT(1) = i;
            if (i & 1u) {
                DOEPTSIZ(1) = (1u << 19) | 64;
                DOEPCTL(1) |= EPENA | CNAK;
            }
        }
    }
    if (st & GI_IEPINT) {
        uint32_t daint = DAINT;
        for (int ep = 0; ep < 2; ep++)
            if (daint & (1u << ep)) DIEPINT(ep) = DIEPINT(ep);
    }
}

void usb_init(void) {
    RCC_AHBENR |= 1u << 12;  /* OTGFS clock (stock: AHB enable 0x1000) */
    delay(1000);
    GUSBCFG |= 1u << 6;      /* on-chip full-speed PHY */
    for (int t = 0; t < 100000 && !(GRSTCTL & (1u << 31)); t++) {}
    GRSTCTL |= 1u;           /* core soft reset */
    for (int t = 0; t < 100000 && (GRSTCTL & 1u); t++) {}
    delay(1000);
    GCCFG = 0x002D0000;
    delay(150000);           /* ~20 ms, as stock */
    GUSBCFG = (GUSBCFG & ~((0xFu << 10) | (3u << 29))) | (5u << 10) | (1u << 30);  /* turnaround 5, force device */
    delay(400000);           /* forced-mode switch takes up to 25 ms */
    PCGCCTL = 0;
    DCTL |= 2u;              /* soft disconnect while setting up */
    DCFG = (DCFG & ~3u) | 3u;  /* full speed, on-chip PHY */

    GRXFSIZ = 128;                     /* words: RX 128, TX0 64 @128, TX1 64 @192 (of 320) */
    DIEPTXF0 = (64u << 16) | 128;
    DIEPTXF1 = (64u << 16) | 192;
    flush_tx(0x10);
    GRSTCTL = 1u << 4;                 /* flush RX FIFO */
    for (int t = 0; t < 100000 && (GRSTCTL & (1u << 4)); t++) {}

    DIEPMSK = DOEPMSK = DAINTMSK = 0;
    for (int i = 0; i < 4; i++) { DIEPINT(i) = 0xFF; DOEPINT(i) = 0xFF; }
    GINTSTS = 0xFFFFFFFF;
    GINTMSK = GI_RXFLVL | GI_USBRST | GI_ENUMDNE | GI_IEPINT | GI_OEPINT;
    GAHBCFG = 1u;                      /* global interrupt enable */
    NVIC_ISER(2) = 1u << (67 - 64);
    DCTL &= ~2u;                       /* connect */
}

int usb_configured(void) { return configured; }

int usb_midi_send(const uint32_t *pkts, int n) {
    int ok = 0;
    __asm volatile("cpsid i");
    if (configured && !(DIEPCTL(1) & EPENA)) {
        DIEPTSIZ(1) = (1u << 19) | (uint32_t)(4 * n);
        DIEPCTL(1) |= EPENA | CNAK;
        for (int i = 0; i < n; i++) FIFO(1) = pkts[i];
        ok = 1;
    }
    __asm volatile("cpsie i");
    return ok;
}
