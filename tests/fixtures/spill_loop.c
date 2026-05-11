/* Adversarial fixture: long, call-heavy loop that stresses spill
 * compaction AND branch relaxation simultaneously.
 *
 * Strategy
 *   - Eight long-lived loop carriers (a..h) and four per-iter temps
 *     (t1..t4) are all live across every call in the body, exercising
 *     the spill-area compaction path: `ever_spilled` ORs every call's
 *     live-after set, and `slot_of[]` packs only those bits into the
 *     frame.
 *   - The body issues 12 noinline calls per iteration. Each call site
 *     spills the live carriers + already-computed temps, sets up
 *     args, and restores after RET — at ~16 spill/restore opcodes per
 *     site that's ~200 spill ops alone, before counting arg setup,
 *     CALL, and the surrounding arithmetic.
 *   - The resulting loop body easily exceeds the imm8 ±127 reach of
 *     the back-edge BEQ/BNE. `cg_relax_branches` must rewrite the
 *     back-edge to `BEQ +1; JMP imm24` for the binary to translate.
 *
 * Helpers `mix` and `blend` are `noinline` so the calls survive
 * clang -O1; inlining would collapse the spill pressure and the
 * branch span and defeat the test.
 *
 * Expected return value captured from a working build of this
 * fixture (n=4). Any change in helper math or carrier update order
 * invalidates the constant.
 */

__attribute__((noinline))
static int mix(int a, int b) {
    return (a * 1103515245) + b * 12345 + 0x5A5A5A5A;
}

__attribute__((noinline))
static int blend(int a, int b, int c) {
    return (a ^ (b + c)) - ((a >> 3) ^ (c << 1));
}

int vm_main(int n) {
    int a = n + 1;
    int b = n + 2;
    int c = n + 3;
    int d = n + 4;
    int e = n + 5;
    int f = n + 6;
    int g = n + 7;
    int h = n + 8;
    int sum = 0;

    for (int i = 0; i < n; i++) {
        int t1 = mix(a, b);
        int t2 = mix(c, d);
        int t3 = mix(e, f);
        int t4 = mix(g, h);
        a = blend(a, t1, b);
        b = blend(b, t2, c);
        c = blend(c, t3, d);
        d = blend(d, t4, e);
        e = blend(e, t1, f);
        f = blend(f, t2, g);
        g = blend(g, t3, h);
        h = blend(h, t4, a);
        sum += a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
    }
    return sum;
}
