/* /bin/fputest SEED: checks that x87/SSE state survives context switches. Two copies
 * run at once (started by the kernel test with different seeds): each loads its own
 * patterns into xmm0-15, yields and busy-loops (so the other copy and timer preemption
 * run in between), then reads the registers back. Also a double-precision loop with
 * yields must give the bit-identical result of the same loop without yields.
 * Exit code: 0 ok, 1 xmm mismatch, 2 double mismatch, 3 x87 mismatch. */
#include "tern.h"

static void xmm_roundtrip(const uint64_t *in, uint64_t *out, long yields, long spins) {
    __asm__ volatile(
        "movdqu 0(%[in]), %%xmm0\n movdqu 16(%[in]), %%xmm1\n"
        "movdqu 32(%[in]), %%xmm2\n movdqu 48(%[in]), %%xmm3\n"
        "movdqu 64(%[in]), %%xmm4\n movdqu 80(%[in]), %%xmm5\n"
        "movdqu 96(%[in]), %%xmm6\n movdqu 112(%[in]), %%xmm7\n"
        "movdqu 128(%[in]), %%xmm8\n movdqu 144(%[in]), %%xmm9\n"
        "movdqu 160(%[in]), %%xmm10\n movdqu 176(%[in]), %%xmm11\n"
        "movdqu 192(%[in]), %%xmm12\n movdqu 208(%[in]), %%xmm13\n"
        "movdqu 224(%[in]), %%xmm14\n movdqu 240(%[in]), %%xmm15\n"
        "1: movl %[nr], %%eax\n syscall\n"
        "   movq %[spins], %%rdx\n"
        "2: decq %%rdx\n jnz 2b\n"
        "   decq %[yields]\n jnz 1b\n"
        "movdqu %%xmm0, 0(%[out])\n movdqu %%xmm1, 16(%[out])\n"
        "movdqu %%xmm2, 32(%[out])\n movdqu %%xmm3, 48(%[out])\n"
        "movdqu %%xmm4, 64(%[out])\n movdqu %%xmm5, 80(%[out])\n"
        "movdqu %%xmm6, 96(%[out])\n movdqu %%xmm7, 112(%[out])\n"
        "movdqu %%xmm8, 128(%[out])\n movdqu %%xmm9, 144(%[out])\n"
        "movdqu %%xmm10, 160(%[out])\n movdqu %%xmm11, 176(%[out])\n"
        "movdqu %%xmm12, 192(%[out])\n movdqu %%xmm13, 208(%[out])\n"
        "movdqu %%xmm14, 224(%[out])\n movdqu %%xmm15, 240(%[out])\n"
        : [yields] "+r"(yields)
        : [in] "r"(in), [out] "r"(out), [nr] "i"(SYS_yield), [spins] "r"(spins)
        : "rax", "rcx", "rdx", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
          "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
          "xmm14", "xmm15");
}

static double series(int n, bool yielding) {
    volatile double one = 1.0;           /* keep the compiler from folding it */
    double acc = 0.0;
    for (int i = 1; i <= n; i++) {
        double x = one / ((double)i * (double)i);
        acc += x;
        if (yielding && i % 64 == 0) yield();
    }
    return acc;
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? (uint64_t)atoi(argv[1]) : 1;
    uint64_t in[32], out[32];
    uint64_t x = seed * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 32; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        in[i] = x;
    }
    for (int round = 0; round < 4; round++) {
        memset(out, 0, sizeof out);
        xmm_roundtrip(in, out, 50, 200000);
        if (memcmp(in, out, sizeof in)) { printf("fputest %lu: xmm mismatch\n", (unsigned long)seed); return 1; }
    }
    double a = series(20000, false), b = series(20000, true);
    if (memcmp(&a, &b, sizeof a)) { printf("fputest %lu: double mismatch\n", (unsigned long)seed); return 2; }
    /* x87: push a value, let others run, pop it */
    long double v = (long double)seed + 0.25L, r;
    __asm__ volatile("fldt %1\n movl %2, %%eax\n syscall\n movl %2, %%eax\n syscall\n fstpt %0"
                     : "=m"(r) : "m"(v), "i"(SYS_yield) : "rax", "rcx", "r11", "memory");
    if (memcmp(&r, &v, 10)) { printf("fputest %lu: x87 mismatch\n", (unsigned long)seed); return 3; }
    printf("fputest %lu: ok, sum %ld/1e9\n", (unsigned long)seed, (long)(a * 1e9));
    return 0;
}
