#include <stdint.h>
/* Completeness gaps: i64/f64 phi (loop-carried), select, i64 variable shifts,
 * llvm.sqrt.f64. Returns 0 on success. n-independent. */
__attribute__((always_inline)) static inline double idn(double x, int n) {
    return x + (double)n - (double)n;
}
static volatile int T = 1, F = 0, SH = 5;

int i64f64_gaps_main(int n) {
    /* i64 phi (loop accumulator) + i64 mul */
    long long acc = 0;
    for (int k = 0; k < 10; ++k) acc += (long long)(k + 1) * 1000000000LL;
    if (acc != 55000000000LL) return 1;

    /* i64 select (both arms) */
    long long a = 0x0000000100000000LL + (long long)n - (long long)n;
    long long b = 0x0000000200000000LL;
    long long s1 = T ? a : b;
    long long s2 = F ? a : b;
    if (s1 != a) return 2;
    if (s2 != b) return 3;

    /* i64 variable-amount shifts */
    int sh = SH;
    unsigned long long u = 0xFFULL;
    if ((u << sh) != 0x1FE0ULL) return 4;
    unsigned long long big = 0xFF00000000ULL;
    if ((big >> sh) != (0xFF00000000ULL >> 5)) return 5;
    long long neg = -0x100000000LL;
    if ((neg >> sh) != (-0x100000000LL >> 5)) return 6;

    /* f64 phi (loop accumulator) -> 10.0 */
    double sum = idn(0.0, n);
    for (int k = 1; k <= 4; ++k) sum += (double)k;
    if (sum != 10.0) return 7;

    /* llvm.sqrt.f64 */
    if (__builtin_sqrt(idn(16.0, n)) != 4.0) return 8;
    double r2 = __builtin_sqrt(idn(2.0, n));
    if (!(r2 > 1.4142135623 && r2 < 1.4142135624)) return 9;

    /* f64 select (both arms) */
    double da = idn(1.5, n), db = idn(2.5, n);
    if ((T ? da : db) != 1.5) return 10;
    if ((F ? da : db) != 2.5) return 11;
    return 0;
}
