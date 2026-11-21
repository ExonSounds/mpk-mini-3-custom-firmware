/* Minimal class-compliant USB-MIDI 1.0 device on the AT32F415 OTGFS core (Synopsys DWC2, slave mode, IRQ 67). */
#pragma once
#include <stdint.h>

void usb_init(void);
int usb_configured(void);
/* Send up to 16 USB-MIDI event packets (byte 0 = cable/CIN, 1 = status, 2-3 = data). 0 = busy or not configured. */
int usb_midi_send(const uint32_t *pkts, int n);
/* Called from the USB interrupt for every packet the host sends (provided by the application). */
void usb_midi_received(uint32_t pkt);
