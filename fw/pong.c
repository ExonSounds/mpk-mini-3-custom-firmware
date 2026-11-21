#include "pong.h"
#include "board.h"

/* Positions are in 1/256 pixel. Screen 128 x 32; paddles 2 x PH at x 2-3 and 124-125; ball 2 x 2. */
#define W 128
#define H 32
#define PH 8
#define FP 256
#define LEFT_X 4             /* right edge of the left paddle */
#define RIGHT_X (W - 4)      /* left edge of the right paddle */
#define PADDLE_SPEED 300     /* ~1.2 px per frame: fast enough to reach most balls, not all */
#define MAX_VX 640

static int bx, by, vx, vy, py[2], aim[2];  /* aim: where on its paddle each side tries to meet the ball (px) */
static unsigned char score[2];
static unsigned pong_rnd = 12345;

static int rnd_n(int n) {
    pong_rnd = pong_rnd * 1103515245u + 12345u;
    return (int)((pong_rnd >> 16) % (unsigned)n);
}

static void new_aims(void) { aim[0] = rnd_n(13) - 6, aim[1] = rnd_n(13) - 6; }  /* a few px off = occasional misses */

static void serve(int dir) {
    bx = (W / 2) * FP, by = (4 + rnd_n(H - 8)) * FP;
    vx = dir * (320 + rnd_n(96));
    vy = (rnd_n(2) ? 1 : -1) * (96 + rnd_n(160));
    new_aims();
}

void pong_reset(void) {
    score[0] = score[1] = 0;
    py[0] = py[1] = (H - PH) / 2 * FP;
    serve(rnd_n(2) ? 1 : -1);
}

static void move_paddle(int side, int active) {
    int target = active ? by + FP - (PH / 2 + aim[side]) * FP : (H - PH) / 2 * FP;  /* idle side drifts to centre */
    int d = target - py[side];
    py[side] += d > PADDLE_SPEED ? PADDLE_SPEED : d < -PADDLE_SPEED ? -PADDLE_SPEED : d;
    if (py[side] < 0) py[side] = 0;
    if (py[side] > (H - PH) * FP) py[side] = (H - PH) * FP;
}

static int hits(int side) { return by + 2 * FP > py[side] && by < py[side] + PH * FP; }

static void bounce(int side) {
    vx = -vx * 17 / 16;  /* a little faster each return */
    if (vx > MAX_VX) vx = MAX_VX;
    if (vx < -MAX_VX) vx = -MAX_VX;
    vy += (by + FP - (py[side] + PH * FP / 2)) / 3;  /* hitting off-centre angles the ball */
    if (vy > 420) vy = 420;
    if (vy < -420) vy = -420;
    if (vy > -48 && vy < 48) vy = vy < 0 ? -48 : 48;  /* never perfectly flat */
    new_aims();
}

static void point(int winner) {
    if (++score[winner] > 9) score[0] = score[1] = 0;  /* single digits: start over after 9 */
    serve(winner ? -1 : 1);                             /* serve towards the side that lost the point */
}

void pong_step(void) {
    move_paddle(0, vx < 0);
    move_paddle(1, vx > 0);
    bx += vx, by += vy;
    if (by < 0) by = -by, vy = -vy;
    if (by > (H - 2) * FP) by = 2 * (H - 2) * FP - by, vy = -vy;
    if (vx < 0 && bx <= LEFT_X * FP && bx > (LEFT_X - 3) * FP && hits(0)) bx = LEFT_X * FP, bounce(0);
    if (vx > 0 && bx + 2 * FP >= RIGHT_X * FP && bx + 2 * FP < (RIGHT_X + 3) * FP && hits(1))
        bx = (RIGHT_X - 2) * FP, bounce(1);
    if (bx < -2 * FP) point(1);
    else if (bx > W * FP) point(0);
}

void pong_draw(void) {
    fb_clear();
    for (int y = 0; y < H; y += 4) rect(W / 2 - 1, y, 1, 2);  /* dotted net */
    digit(W / 2 - 9, 1, score[0], 1);
    digit(W / 2 + 6, 1, score[1], 1);
    rect(LEFT_X - 2, py[0] / FP, 2, PH);
    rect(RIGHT_X, py[1] / FP, 2, PH);
    rect(bx / FP, by / FP, 2, 2);
}
