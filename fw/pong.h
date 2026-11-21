/* Idle screen: the keyboard plays ping-pong against itself (two computer paddles, a ball, a score). */
#pragma once

void pong_reset(void);  /* new game, scores 0:0 */
void pong_step(void);   /* advance one frame (~33 ms) */
void pong_draw(void);   /* draw into fb; the caller flushes */
