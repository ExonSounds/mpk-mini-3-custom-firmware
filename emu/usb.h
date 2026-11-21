/* USB inside the emulator: usb.c = the chip's DWC2 OTG FS core (device mode), usbhost.c = the computer's USB host
 * (enumeration, 1 ms frames, endpoint polling), midi_bridge.c = the computer side (ALSA sequencer port + PTY).
 * All functions run on the emulator thread unless they say "any thread". */
#pragma once
#include <stdint.h>

/* ---- device side (usb.c): one transaction each, as the host sees it on the wire ---- */
enum { USB_ACK, USB_NAK, USB_STALL };
int usb_dev_setup(const uint8_t setup[8]);             /* SETUP to EP0 (always ACKed unless the RX FIFO is full) */
int usb_dev_in(int ep, uint8_t *buf, int *len);       /* IN token: up to the endpoint's max packet into buf */
int usb_dev_out(int ep, const uint8_t *buf, int len); /* OUT token + data */
void usb_dev_bus_reset(int start);                    /* 1 = reset begins (USBRST), 0 = reset ends (ENUMDNE) */
void usb_dev_sof(unsigned frame);

/* ---- host (usbhost.c) ---- */
void usbhost_connect(int on);  /* D+ pull-up changed (soft connect/disconnect, PHY power, chip reset) */
void usbhost_kick(void);       /* computer -> device MIDI is waiting: poll bulk OUT soon */

/* ---- computer side (midi_bridge.c) ---- */
void midi_attach(const char *product, int hid);  /* configured: create the ALSA port + PTY(s), publish them */
void midi_detach(void);                 /* device gone: remove them */
void midi_from_device(uint32_t pkt);    /* one USB-MIDI event packet from the bulk IN endpoint (little-endian) */
int midi_to_device(uint32_t *pkts, int max);  /* takes up to max queued packets for the bulk OUT endpoint */
void hid_from_device(const uint8_t *buf, int n);  /* one input report from the HID interrupt IN endpoint */
int hid_to_device(uint8_t *buf);                  /* one queued 32-byte output report (returns its size) or 0 */
