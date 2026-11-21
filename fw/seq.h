/* Tempo clock (96 PPQN, internal), arpeggiator for keys, note repeat for pads, tap tempo.
 * Everything runs in the SysTick context, like the inputs that feed it. */
#pragma once
#include <stdint.h>

enum { ARP_UP, ARP_DOWN, ARP_EXC, ARP_INC, ARP_ORDER, ARP_RANDOM };  /* same order as the stock editor */

extern volatile uint8_t seq_arp_on, seq_repeat_on, seq_beat_led;

void seq_tick(void);                          /* every SysTick (20 kHz) */
void seq_tap(void);                           /* TAP TEMPO pressed: sets S.tempo from the last taps */
void seq_realtime(uint8_t b);
int seq_bpm(void);                            /* tempo now: S.tempo, or measured from MIDI clock (0 = none) */                 /* realtime byte from the host: F8 clock, FA/FB/FC start/continue/stop */
void seq_arp_toggle(void);
void arp_key(int note, int vel, int on);      /* key routed to the arpeggiator */
void repeat_pad(int pad, int note, int vel, int on);  /* pad routed to note repeat */
