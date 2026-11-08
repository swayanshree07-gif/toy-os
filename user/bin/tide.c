/* /bin/tide [SEED]: a small original grid puzzle (Day 19, own design).
 *
 * You (@) stand on an island of heights 0..9 (shown as digits). The tide starts at
 * level 0 and rises by one every TIDE_EVERY moves; a cell is water (~) once its height
 * is <= the tide level. Collect every shell (*) and then climb the lighthouse (L, the
 * highest cell) before the sea catches you. If the tide reaches the cell you stand on,
 * you drown.
 *
 * Input is read from fd 0, a line at a time; every character is one command:
 *   w a s d   move up / left / down / right      .  wait one move
 *   ?         hint: print the next move of a shortest solution
 *   m         print the map                       q  give up
 * Same SEED → same island, so a game can be scripted. Generation retries until the
 * built-in breadth-first solver finds a solution, so every island is winnable.
 * Exit status: 0 won, 1 drowned/gave up, 2 bad usage.
 *
 * Built for the host too (cc -DTIDE_HOST user/bin/tide.c) to find scripted solutions. */
#ifdef TIDE_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#else
#include "tern.h"
#endif

#define W 12
#define H 8
#define N (W * H)
#define SHELLS 3
#define TIDE_EVERY 5
#define MAXT (10 * TIDE_EVERY)       /* by then everything is under water */

static uint8_t height[N];
static int shell_at[SHELLS];
static int light, start;
static uint32_t rng;

static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7fff; }

static int level_at(int moves) { return moves / TIDE_EVERY; }
static bool wet(int c, int moves) { return height[c] <= level_at(moves); }

static void generate(uint32_t seed) {
    rng = seed * 2654435761u + 1;
    int raw[N];
    for (int i = 0; i < N; i++) raw[i] = (int)(rnd() % 10);
    /* one hill: cells near a random centre get raised, then smooth once */
    int cx = 3 + (int)(rnd() % (W - 6)), cy = 2 + (int)(rnd() % (H - 4));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int d = (x > cx ? x - cx : cx - x) + (y > cy ? y - cy : cy - y);
            raw[y * W + x] += 12 - 2 * d;
        }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int s = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    s += raw[yy * W + xx]; n++;
                }
            int v = s / n;
            height[y * W + x] = (uint8_t)(v < 0 ? 0 : v > 9 ? 9 : v);
        }
    light = 0;
    for (int i = 1; i < N; i++) if (height[i] > height[light]) light = i;
    /* start: a random cell of height 3..5; shells: distinct cells of height >= 2 */
    do start = (int)(rnd() % N); while (height[start] < 3 || height[start] > 5 || start == light);
    for (int k = 0; k < SHELLS; k++) {
        int c; bool dup;
        do {
            c = (int)(rnd() % N);
            dup = c == start || c == light;
            for (int j = 0; j < k; j++) dup |= shell_at[j] == c;
        } while (dup || height[c] < 2);
        shell_at[k] = c;
    }
}

/* ---- breadth-first solver over (moves, cell, shells mask) ---- */
static const int DX[5] = { 0, -1, 0, 1, 0 }, DY[5] = { -1, 0, 1, 0, 0 };
static const char KEY[5] = { 'w', 'a', 's', 'd', '.' };
static int16_t prev_state[MAXT + 1][N][1 << SHELLS];   /* -1 unseen */
static int8_t prev_dir[MAXT + 1][N][1 << SHELLS];

static int pick(int c, int mask) {
    for (int k = 0; k < SHELLS; k++) if (shell_at[k] == c) mask |= 1 << k;
    return mask;
}

/* Finds a shortest winning plan from (t0, c0, m0); writes up to max keys into out
 * (if not NULL). Returns its length or -1. */
static int solve(int t0, int c0, int m0, char *out, int max) {
    memset(prev_state, 0xff, sizeof prev_state);
    const int full = (1 << SHELLS) - 1;
    prev_state[t0][c0][m0] = (int16_t)(N * 8);  /* root marker */
    for (int t = t0; t < MAXT; t++)
        for (int c = 0; c < N; c++)
            for (int m = 0; m <= full; m++) {
                if (prev_state[t][c][m] < 0) continue;
                if (c == light && m == full) {
                    /* walk back */
                    int len = t - t0, ct = t, cc = c, cm = m;
                    while (ct > t0) {
                        int d = prev_dir[ct][cc][cm], ps = prev_state[ct][cc][cm];
                        if (out && ct - t0 - 1 < max) out[ct - t0 - 1] = KEY[d];
                        cc = ps / 8; cm = ps % 8; ct--;
                    }
                    return len;
                }
                for (int d = 0; d < 5; d++) {
                    int x = c % W + DX[d], y = c / W + DY[d];
                    if (x < 0 || y < 0 || x >= W || y >= H) continue;
                    int nc = y * W + x;
                    if (wet(nc, t) || wet(nc, t + 1)) continue;  /* no stepping into water / drowning */
                    int nm = pick(nc, m);
                    if (prev_state[t + 1][nc][nm] >= 0) continue;
                    prev_state[t + 1][nc][nm] = (int16_t)(c * 8 + m);
                    prev_dir[t + 1][nc][nm] = (int8_t)d;
                }
            }
    return -1;
}

/* ---- game ---- */
static int pos, moves, mask;

static void draw(void) {
    printf("tide: level %d, move %d, shells %d/%d\n", level_at(moves), moves,
           __builtin_popcount((unsigned)mask), SHELLS);
    for (int y = 0; y < H; y++) {
        char row[W + 2];
        for (int x = 0; x < W; x++) {
            int c = y * W + x;
            char ch = wet(c, moves) ? '~' : (char)('0' + height[c]);
            for (int k = 0; k < SHELLS; k++) if (shell_at[k] == c && !(mask >> k & 1)) ch = '*';
            if (c == light) ch = 'L';
            if (c == pos) ch = '@';
            row[x] = ch;
        }
        row[W] = '\n'; row[W + 1] = 0;
        printf("  %s", row);
    }
}

static char ibuf[256];
static long ilen, ipos;
static int getch(void) {
    if (ipos == ilen) {
        ilen = read(0, ibuf, sizeof ibuf); ipos = 0;
        if (ilen <= 0) { ilen = 0; return -1; }
    }
    return (unsigned char)ibuf[ipos++];
}

int main(int argc, char **argv) {
    uint32_t seed = 1;
    bool host_solve = false;
    if (argc > 1) seed = (uint32_t)atoi(argv[1]);
    if (argc > 2 && !strcmp(argv[2], "-solve")) host_solve = true;
    if (argc > 3 || (argc > 2 && !host_solve)) { printf("usage: tide [SEED]\n"); return 2; }

    uint32_t s = seed;
    int tries = 0;
    for (;;) {
        generate(s);
        if (solve(0, start, 0, NULL, 0) >= 0) break;
        s = s * 31 + 7; tries++;
    }
    if (host_solve) {
        static char plan[MAXT + 1];
        int n = solve(0, start, 0, plan, MAXT);
        plan[n] = 0;
        printf("%s\n", plan);
        return 0;
    }
    pos = start; moves = 0; mask = pick(start, 0);
    printf("tide: seed %u (island %d), collect %d shells, then reach L. wasd . ? m q\n",
           seed, tries, SHELLS);
    draw();
    const int full = (1 << SHELLS) - 1;
    for (;;) {
        int ch = getch();
        if (ch < 0 || ch == 'q') { printf("tide: gave up after %d moves\n", moves); return 1; }
        if (ch == '\n' || ch == '\r' || ch == ' ') continue;
        if (ch == 'm') { draw(); continue; }
        if (ch == '?') {
            char k[1];
            int n = solve(moves, pos, mask, k, 1);
            if (n > 0) printf("tide: hint %c (%d moves to win)\n", k[0], n);
            else printf("tide: hint: no way out any more\n");
            continue;
        }
        int d = -1;
        for (int i = 0; i < 5; i++) if (KEY[i] == ch) d = i;
        if (d < 0) { printf("tide: unknown key '%c'\n", ch); continue; }
        int x = pos % W + DX[d], y = pos / W + DY[d];
        if (x < 0 || y < 0 || x >= W || y >= H) { printf("tide: edge of the map\n"); continue; }
        int nc = y * W + x;
        if (wet(nc, moves)) { printf("tide: splash, that is water\n"); continue; }
        int oldlevel = level_at(moves);
        pos = nc; moves++;
        int nm = pick(pos, mask);
        if (nm != mask) printf("tide: shell! %d/%d\n", __builtin_popcount((unsigned)nm), SHELLS);
        mask = nm;
        if (level_at(moves) != oldlevel) printf("tide: the sea rises to %d\n", level_at(moves));
        if (wet(pos, moves)) {
            draw();
            printf("tide: DROWNED at move %d with %d/%d shells\n", moves,
                   __builtin_popcount((unsigned)mask), SHELLS);
            return 1;
        }
        if (pos == light && mask == full) {
            draw();
            printf("tide: WON seed %u in %d moves, %d/%d shells, tide %d\n", seed, moves,
                   SHELLS, SHELLS, level_at(moves));
            return 0;
        }
    }
}
