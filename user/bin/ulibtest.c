/* /bin/ulibtest: the user library (printf family, malloc on brk). Exits 0 on success,
 * otherwise with the number of the failed check. */
#include "tern.h"

static int check_no;
#define CHECK(c) do { check_no++; if (!(c)) { \
    printf("ulibtest: check %d failed: %s\n", check_no, #c); exit(check_no); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

#define SLOTS 64
static uint8_t *slot[SLOTS];
static size_t slot_len[SLOTS];

int main(void) {
    char b[96];
    /* printf formatting: return value is the full length, output matches */
#define FMT(want, ...) CHECK(snprintf(b, sizeof b, __VA_ARGS__) == (int)strlen(want) && \
                             strcmp(b, want) == 0)
    FMT("-42|    7|3   |-0012|4000000000", "%d|%5d|%-4d|%05d|%u", -42, 7, 3, -12, 4000000000u);
    FMT("ff ABC 123456789abcdef 0x400000", "%x %X %lx %p", 255u, 0xABCu,
        0x123456789abcdefUL, (void *)0x400000);
    FMT("ab/Z/%/(null)", "%s/%c/%%/%s", "ab", 'Z', (char *)0);
    FMT("-9223372036854775808", "%ld", (long)-9223372036854775807L - 1);
    CHECK(snprintf(b, 5, "%s", "truncated") == 9 && strcmp(b, "trun") == 0);

    /* brk */
    long base = brk(0);
    CHECK(base >= 0x400000 && base % 4096 == 0);
    CHECK(brk(base + (long)USER_HEAP_MAX + 1) == base);          /* over the limit */
    CHECK(brk(base - 4096) == base);                             /* below the image */
    CHECK(brk(base + 10000) == base + 10000);
    volatile uint8_t *h = (uint8_t *)base;
    CHECK(h[0] == 0 && h[9999] == 0);
    h[0] = 1; h[9999] = 2;                                       /* writable */
    CHECK(brk(base) == base);                                    /* shrink back */

    /* malloc: random allocations with fill patterns, then free everything */
    for (int round = 0; round < 3000; round++) {
        int i = (int)(rnd() % SLOTS);
        if (slot[i]) {
            for (size_t k = 0; k < slot_len[i]; k++)
                if (slot[i][k] != (uint8_t)(i + k)) { printf("ulibtest: slot %d corrupted\n", i); exit(100); }
            if (rnd() % 4 == 0) {
                size_t nl = slot_len[i] + rnd() % 300;
                uint8_t *q = realloc(slot[i], nl);
                CHECK(q != NULL);
                for (size_t k = slot_len[i]; k < nl; k++) q[k] = (uint8_t)(i + k);
                slot[i] = q; slot_len[i] = nl;
            } else {
                free(slot[i]); slot[i] = NULL;
            }
        } else {
            size_t n = rnd() % 3 == 0 ? 1 + rnd() % 20000 : 1 + rnd() % 200;
            slot[i] = malloc(n);
            CHECK(slot[i] != NULL && ((uintptr_t)slot[i] & 15) == 0);
            slot_len[i] = n;
            for (size_t k = 0; k < n; k++) slot[i][k] = (uint8_t)(i + k);
        }
    }
    for (int i = 0; i < SLOTS; i++) free(slot[i]);
    size_t nb, by;
    malloc_stats(&nb, &by);
    CHECK(nb == 1);                                  /* everything coalesced */
    uint8_t *big = malloc(1 << 20);                  /* grows the heap by 1 MiB */
    CHECK(big != NULL);
    memset(big, 0x5A, 1 << 20);
    free(big);
    int *z = calloc(1000, sizeof(int));
    CHECK(z != NULL);
    for (int i = 0; i < 1000; i++) CHECK(z[i] == 0);
    free(z);
    CHECK(malloc((size_t)USER_HEAP_MAX + 1) == NULL);
    malloc_stats(&nb, &by);
    printf("[TEST] ulibc ok: printf formats, brk, 3000 malloc/realloc/free ops, "
           "heap %lu KiB in %lu free block(s)\n", (unsigned long)(by >> 10), (unsigned long)nb);
    return 0;
}
