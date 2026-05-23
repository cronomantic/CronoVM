#include <stdint.h>

/* 64-bit calling convention (phase 3): i64/f64 as function arguments and
 * return values, across real calls. Each helper is noinline so it stays a
 * genuine call exercising the ABI (a 64-bit value = two argument words; the
 * return comes back in R0:R1). The entry takes/returns i32 so test_e2e can
 * drive it. Returns 0 on success, else the first failed check. n-neutral. */

__attribute__((noinline)) static long long add64(long long a, long long b) { return a + b; }
__attribute__((noinline)) static long long mul64(long long a, long long b) { return a * b; }
__attribute__((noinline)) static double    fadd2(double a, double b)       { return a + b; }
__attribute__((noinline)) static double    fscale(double x, int k)         { return x * (double)k; }
/* mixed scalar/wide/scalar args (word positions: a=0, b=1,2, c=3). */
__attribute__((noinline)) static long long mix(int a, long long b, int c)  { return (long long)a + b + (long long)c; }
/* five i64 args = 10 words: words 0..7 in R0..R7, the 5th arg straddles to
 * the stack (words 8,9). */
__attribute__((noinline)) static long long many(long long a, long long b, long long c,
                                                 long long d, long long e) { return a + b + c + d + e; }

int abi64_main(int n) {
    long long z = (long long)n - (long long)n;          /* 0, blocks folding */
    double    dz = (double)n - (double)n;

    if (add64(0x100000000LL + z, 7LL)       != 0x100000007LL) return 1;
    if (mul64(0x100000000LL + z, 3LL)       != 0x300000000LL) return 2;
    if (fadd2(1.5 + dz, 2.0)                != 3.5)           return 3;
    if (fscale(2.5 + dz, 4)                 != 10.0)          return 4;
    if (mix(10, 0x100000000LL + z, 5)       != 0x10000000FLL) return 5;
    if (many(1LL + z, 2LL, 3LL, 4LL, 5LL)   != 15LL)          return 6;

    /* nested: a wide call result feeds another wide call's arg */
    if (add64(mul64(0x100000000LL + z, 2LL), 1LL) != 0x200000001LL) return 7;

    /* f64 round-trip through two calls */
    if (fadd2(fscale(1.0 + dz, 3), 0.5)     != 3.5)           return 8;
    return 0;
}
