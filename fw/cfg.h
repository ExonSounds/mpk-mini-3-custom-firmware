/* Configuration over USB-MIDI SysEx, used by the desktop GUI (mpk3_cfg.py).
 *   host -> keyboard:  F0 7D 4D 4B <cmd> [data] F7        (7D = non-commercial manufacturer ID, "MK")
 *   keyboard -> host:  F0 7D 4D 4B <cmd | 0x40> [data] F7
 * cmd 01 HELLO    -> fw version hi, lo, settings layout, settings size
 *     02 GET      -> settings bytes as nibbles (high, low)
 *     03 SET      <- settings nibbles; applied at once (not saved)      -> status
 *     04 SAVE     -> status (written to flash and verified)
 *     05 DEFAULTS -> status (applied, not saved)
 *     06 IMAGE_WRITE <- offset hi, lo (7 bits each, multiple of 64), 64 bytes as nibbles -> status (save with SAVE)
 *     07 IMAGE_READ  <- offset hi, lo -> image-set flag, 64 bytes as nibbles
 *     08 IMAGE_CLEAR -> status (idle image back to the built-in logo; save with SAVE)
 *     09 PROG_WRITE  <- block hi, lo (64-byte blocks), 64 bytes as nibbles -> status   (RAM program: prog_api.h)
 *     0A PROG_RUN    <- size, sum16 (3 x 7 bits each) -> status, then the program starts
 * status: 0 ok, 1 wrong length, 2 invalid values / save failed / bad program, 3 busy (a program is running),
 *         0x7F unknown command */
#pragma once
#include <stdint.h>

void cfg_rx(uint32_t pkt);  /* from the USB interrupt: one USB-MIDI packet with a SysEx CIN (4-7) */
void cfg_poll(void);        /* from the main loop: runs a complete command */
