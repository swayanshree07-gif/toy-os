/* /bin/demo: "tern flight", a framebuffer animation (Day 11 session).
 *   demo [frames]   take over the screen and animate (default 300 frames, 0 = until ^C)
 *   demo --test     checks of the framebuffer syscalls, then shows a fixed test pattern
 *                   for a moment and exits without releasing (the kernel must clean up)
 * Everything is drawn into a private buffer and copied to the screen with fbblit. */
#include "tern.h"

#define CHECK(c) do { if (!(c)) { printf("demo: check failed line %d: %s\n", __LINE__, #c); return __LINE__; } } while (0)

/* ---- sine table (Q14 fixed point, 1024 steps per turn), built with a Taylor series ---- */
#define SIN_N 1024
static int sin_tab[SIN_N];

static void build_sin(void) {
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < SIN_N; i++) {
        double x = 2 * pi * i / SIN_N;
        if (x > pi) x -= 2 * pi;                       /* [-pi, pi] */
        double t = x, s = 0, x2 = x * x;
        for (int k = 1; k < 20; k += 2) { s += t; t *= -x2 / ((k + 1) * (k + 2)); }
        sin_tab[i] = (int)(s * 16384 + (s < 0 ? -0.5 : 0.5));
    }
}
static inline int isin(int a) { return sin_tab[a & (SIN_N - 1)]; }      /* -16384..16384 */

/* ---- sprites: original pixel art, a tern seen from the side, two wing positions ---- */
static const char *const tern_up[] = {
    "..........gg",
    ".........ggg",
    "........gggg",
    ".......ggggg",
    "......gggggg.......kk",
    "w.....ggggggw....wkkkoo",
    "ww...wwwwwwwwwwwwwwkk",
    ".wwwwwwwwwwwwwwwwwww",
    "w...wwwwwwwwwwwwww",
    "..........ww",
    0,
};
static const char *const tern_down[] = {
    "",
    "",
    "",
    "",
    "...................kk",
    "w................wkkkoo",
    "ww...wwwwwwwwwwwwwwkk",
    ".wwwwwwwggggggwwwww",
    "w...wwwwggggggwwww",
    ".........ggggg",
    "..........gggg",
    "...........ggg",
    "............gg",
    0,
};

static uint32_t *buf;
static uint32_t W, H;

static inline uint32_t rgb(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void sprite(const char *const *rows, int x0, int y0, int scale) {
    for (int r = 0; rows[r]; r++)
        for (int c = 0; rows[r][c]; c++) {
            uint32_t col;
            switch (rows[r][c]) {
            case 'w': col = rgb(250, 250, 245); break;
            case 'g': col = rgb(160, 170, 185); break;
            case 'k': col = rgb(20, 20, 25); break;
            case 'o': col = rgb(240, 120, 30); break;
            default: continue;
            }
            for (int dy = 0; dy < scale; dy++) {
                int y = y0 + r * scale + dy;
                if (y < 0 || y >= (int)H) continue;
                for (int dx = 0; dx < scale; dx++) {
                    int x = x0 + c * scale + dx;
                    if (x >= 0 && x < (int)W) buf[(uint32_t)y * W + (uint32_t)x] = col;
                }
            }
        }
}

static void render(int t) {
    uint32_t horizon = H * 2 / 3;
    /* sky: vertical gradient from deep blue to a warm haze at the horizon */
    for (uint32_t y = 0; y < horizon; y++) {
        int k = (int)(y * 256 / horizon);
        uint32_t c = rgb(20 + k * 200 / 256, 40 + k * 120 / 256, 110 + k * 60 / 256);
        uint32_t *row = buf + y * W;
        for (uint32_t x = 0; x < W; x++) row[x] = c;
    }
    /* sun, slowly sinking */
    int sr = (int)H / 10, scx = (int)W * 3 / 4, scy = (int)horizon - sr / 2 + (t / 8) % (sr / 2 + 1);
    for (int y = scy - sr; y <= scy + sr; y++) {
        if (y < 0 || y >= (int)horizon) continue;
        for (int x = scx - sr; x <= scx + sr; x++) {
            int dx = x - scx, dy = y - scy;
            if (x >= 0 && x < (int)W && dx * dx + dy * dy <= sr * sr)
                buf[(uint32_t)y * W + (uint32_t)x] = rgb(255, 200 - (dy + sr) * 40 / sr, 90);
        }
    }
    /* sea: moving interference of two waves, brighter crests, sun glitter column */
    for (uint32_t y = horizon; y < H; y++) {
        int depth = (int)(y - horizon);
        uint32_t *row = buf + y * W;
        for (uint32_t x = 0; x < W; x++) {
            int v = isin((int)x * 3 + t * 9 + depth * 11) + isin((int)x * 7 / 5 - t * 6 + depth * 23);
            int light = v >> 10;                        /* -32..32 */
            int glint = 0;
            int d = (int)x - scx;
            int ad = d < 0 ? -d : d;
            if (ad < sr && v > 20000)                    /* glitter, fading away from the sun */
                glint = (120 - depth * 120 / (int)(H - horizon + 1)) * (sr - ad) / sr;
            row[x] = rgb(10 + light + glint, 60 + depth / 4 + light + glint, 110 + depth / 3 + light * 2 + glint / 2);
        }
    }
    /* three terns on wavy paths at different speeds and sizes */
    for (int i = 0; i < 3; i++) {
        int scale = 6 - i * 2 + (W >= 1600 ? 2 : 0);
        int span = (int)W + 30 * scale;
        int x = (t * (5 - i) + i * span / 3) % span - 25 * scale;
        int y = (int)horizon / (4 - i) + (isin(t * 12 + i * 300) * (int)(H / 16) >> 14);
        sprite(((t / 6 + i) & 1) ? tern_up : tern_down, x, y, scale);
    }
}

static int show(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct tern_fbblit b = { buf + y * W + x, x, y, w, h, W * 4, 0 };
    return fbblit(&b);
}

/* --- API checks (run by the kernel's boot test) --- */
static int run_tests(void) {
    struct tern_fbinfo fi;
    CHECK(fbinfo(&fi) == 0 && fi.width >= 320 && fi.height >= 200 && fi.owner == 0);
    CHECK(fbinfo((struct tern_fbinfo *)0xffffffff80000000ull) == -EFAULT);
    W = fi.width; H = fi.height;
    buf = malloc((size_t)W * H * 4);
    CHECK(buf);
    struct tern_fbblit b = { buf, 0, 0, 8, 8, 32, 0 };
    CHECK(fbblit(&b) == -EPERM);                         /* not the owner yet */
    CHECK(fbacquire(0) == -EPERM);                       /* nothing to release */
    CHECK(fbacquire(7) == -EINVAL);
    CHECK(fbacquire(1) == 0);
    CHECK(fbacquire(1) == 0);                            /* already ours */
    CHECK(fbinfo(&fi) == 0 && fi.owner == (uint32_t)getpid());
    /* another process can't take it */
    const char *cargv[] = { "demo", "--try", NULL };
    int pid = spawn("/bin/demo", cargv), st = -1;
    CHECK(pid > 0 && wait(pid, &st) == pid && st == 0);
    /* bad rectangles and buffers */
    b = (struct tern_fbblit){ buf, W - 4, 0, 8, 1, 32, 0 };
    CHECK(fbblit(&b) == -EINVAL);                        /* off the right edge */
    b = (struct tern_fbblit){ buf, 0, H, 1, 1, 4, 0 };
    CHECK(fbblit(&b) == -EINVAL);
    b = (struct tern_fbblit){ buf, 0, 0, 0xffffffffu, 1, 4, 0 };
    CHECK(fbblit(&b) == -EINVAL);
    b = (struct tern_fbblit){ buf, 0, 0, 16, 2, 32, 0 };
    CHECK(fbblit(&b) == -EINVAL);                        /* stride < 4 * w */
    b = (struct tern_fbblit){ (const uint32_t *)0xffffffff80100000ull, 0, 0, 4, 4, 16, 0 };
    CHECK(fbblit(&b) == -EFAULT);                        /* kernel memory */
    b = (struct tern_fbblit){ (const uint32_t *)0x10000000ull, 0, 0, 4, 4, 16, 0 };
    CHECK(fbblit(&b) == -EFAULT);                        /* unmapped */
    b = (struct tern_fbblit){ buf, 0, 0, 4, H, 1u << 20, 0 };
    CHECK(fbblit(&b) == -EFAULT);                        /* rows run far past the heap */
    CHECK(fbblit((const struct tern_fbblit *)8) == -EFAULT);
    b = (struct tern_fbblit){ buf, 0, 0, 0, 0, 0, 0 };
    CHECK(fbblit(&b) == 0);                              /* empty is fine */
    /* the test pattern the kernel looks for */
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            buf[y * W + x] = ((x * 7) ^ (y * 13)) & 0xffffff;
    CHECK(show(0, 0, W, H) == 0);
    printf("demo: test pattern up\n");
    sleep_ms(600);
    return 0;                                            /* exit while still owning it */
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--try")) {
        struct tern_fbblit b = { 0, 0, 0, 1, 1, 4, 0 };
        if (fbacquire(1) != -EBUSY) return 1;
        if (fbacquire(0) != -EPERM) return 2;
        if (fbblit(&b) != -EPERM) return 3;
        return 0;
    }
    build_sin();
    if (argc > 1 && !strcmp(argv[1], "--test")) return run_tests();

    int frames = argc > 1 ? atoi(argv[1]) : 300;
    struct tern_fbinfo fi;
    int rc = fbinfo(&fi);
    if (rc) { printf("demo: no framebuffer (%d)\n", rc); return 1; }
    W = fi.width; H = fi.height;
    buf = malloc((size_t)W * H * 4);
    if (!buf) { printf("demo: out of memory for a %ux%u buffer\n", W, H); return 1; }
    if ((rc = fbacquire(1))) { printf("demo: framebuffer busy (%d)\n", rc); return 1; }
    uint64_t t0 = uptime_ms(), next = t0;
    int n = 0;
    for (; frames == 0 || n < frames; n++) {
        render(n);
        if ((rc = show(0, 0, W, H))) { fbacquire(0); printf("demo: fbblit failed (%d)\n", rc); return 1; }
        if (n == 0) printf("demo: running (%ux%u)\n", W, H);
        next += 33;                                      /* ~30 frames per second at most */
        uint64_t now = uptime_ms();
        if (now < next) sleep_ms(next - now);
        else next = now;
    }
    uint64_t ms = uptime_ms() - t0;
    fbacquire(0);
    uint64_t fps10 = ms ? (uint64_t)n * 10000 / ms : 0;
    printf("demo: %d frames in %lu ms (%lu.%lu fps)\n", n, ms, fps10 / 10, fps10 % 10);
    return 0;
}
