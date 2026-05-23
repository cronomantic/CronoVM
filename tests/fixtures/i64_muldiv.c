#include <stdint.h>

/* i64 multiply (inline MUL/MULHU) + divide/remainder (soft runtime call into
 * cvm_int64_rt). Operands are volatile so clang can't fold the 64-bit
 * arithmetic or strength-reduce the divides to shifts — the real ops reach
 * codegen. Returns 0 on success, else the first failed check. n-neutral. */
static volatile long long  A = 0x0000000100000007LL;   /* 2^32 + 7 */
static volatile long long  B = 3LL;
static volatile long long  N = -1000000000007LL;       /* big negative */
static volatile long long  D = 7LL;
static volatile unsigned long long U  = 0xFFFFFFFF00000000ULL;
static volatile unsigned long long UD = 0x0000000300000000ULL;   /* 3 * 2^32 */

int i64_muldiv_main(int n) {
    long long a = A + (long long)n - (long long)n;
    long long b = B, d = D, nn = N;

    /* multiply crossing the 32-bit boundary */
    if (a * b != 0x0000000300000015LL) return 1;        /* (2^32+7)*3 */
    if (a * a != 0x0000000E00000031LL) return 2;        /* (2^32+7)^2 mod 2^64 */

    /* unsigned divide / remainder (runtime; non-power-of-2 divisor) */
    unsigned long long u = U, ud = UD;
    if (u / ud != 0x55555555ULL) return 3;
    if (u % ud != (u - (u / ud) * ud)) return 4;

    /* signed divide / remainder, negative dividend (trunc toward zero) */
    if (nn / d != -142857142858LL) return 5;
    if (nn % d != -1LL) return 6;

    /* round-trip: (a*b)/b == a, (a*b)%b == 0 */
    long long ab = a * b;
    if (ab / b != a) return 7;
    if (ab % b != 0) return 8;

    /* negative divisor */
    if (nn / (-d) != 142857142858LL) return 9;
    if (nn % (-d) != -1LL) return 10;

    return 0;
}
