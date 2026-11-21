/* RAM programs (API for programs: prog_api.h). Upload with SysEx 09/0A (cfg.h), run from the main loop. */
#pragma once
#include <stdint.h>

extern volatile uint8_t prog_running;    /* a program is running (from prog_exec) */
extern volatile uint8_t prog_own_input;  /* the running program handles keys/pads/knobs/buttons itself */
extern volatile uint8_t prog_run_req;    /* set by prog_start: main loop calls prog_exec */

uint8_t prog_write(unsigned off, const uint8_t *nibbles);  /* 64 bytes at off -> cfg status (3 = busy) */
uint8_t prog_start(unsigned size, unsigned sum16);         /* check what was uploaded -> cfg status */
void prog_exec(void);                                      /* main loop: run it until it returns */
