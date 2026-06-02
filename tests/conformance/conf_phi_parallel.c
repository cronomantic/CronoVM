/* Parallel-copy (phi-move) sequentialization on a busy loop-carried edge.
 *
 * A back-edge that copies one loop-carried value DIRECTLY into another
 * (`a = b;` with both phi-homed) makes the edge's phi moves a parallel copy
 * with conflicts: a destination is also a source. cg_emit_phi_moves resolves
 * these. The historic code read every source into its OWN scratch register
 * before writing any destination, so an edge with more than ~23 conflicting
 * moves exhausted the 23-register emit scratch window ("emit scratch window
 * exhausted") — exactly Exult's BilinearScalerInternal_2x, a loop carrying
 * hundreds of values. The translator now sequentializes the parallel copy with
 * O(1) scratch (one spare register breaks cycles, reused across them).
 *
 * This fixture builds TWO independent cyclic rotations in one loop body — two
 * disjoint permutation cycles, each longer than the old window, so together
 * they (a) far exceed 23 conflicting moves on the back-edge and (b) require the
 * cycle-breaking spare to be REUSED across cycles (the property that makes one
 * spare sufficient). A position-sensitive hash (`acc`) is folded in every
 * iteration so a wrong move ORDER — not just a wrong count — changes the
 * checksum. The arrays are constant-indexed and never address-taken, so the
 * front end promotes them to scalars (phis); the rotation is written fully
 * unrolled to keep every index a compile-time constant.
 *
 * The trip count is read through a volatile so the loop is neither unrolled
 * away nor constant-folded; the rotations therefore survive as real phi cycles.
 * All math is on `unsigned` (defined overflow) folded to an int32 checksum, so
 * the VM-32 result matches the native-64 oracle bit-for-bit.
 */

static volatile int g_trips = 101;

/* one left-cyclic-rotation of v[0..N-1]: v[k] <- v[k+1], v[N-1] <- old v[0]. */
#define ROT15(v)                                                    \
    do {                                                            \
        unsigned _t = v##0;                                         \
        v##0 = v##1;  v##1 = v##2;  v##2 = v##3;  v##3 = v##4;       \
        v##4 = v##5;  v##5 = v##6;  v##6 = v##7;  v##7 = v##8;       \
        v##8 = v##9;  v##9 = v##10; v##10 = v##11; v##11 = v##12;    \
        v##12 = v##13; v##13 = v##14; v##14 = _t;                   \
    } while (0)

int conf_main(void)
{
    int n = g_trips;

    /* group A */
    unsigned a0=3,a1=5,a2=7,a3=11,a4=13,a5=17,a6=19,a7=23,
             a8=29,a9=31,a10=37,a11=41,a12=43,a13=47,a14=53;
    /* group B */
    unsigned b0=59,b1=61,b2=67,b3=71,b4=73,b5=79,b6=83,b7=89,
             b8=97,b9=101,b10=103,b11=107,b12=109,b13=113,b14=127;

    unsigned acc = 2166136261u;
    for (int i = 0; i < n; i++)
    {
        ROT15(a);                 /* a pure 15-cycle of phi<-phi copies */
        ROT15(b);                 /* a second, disjoint 15-cycle */

        /* Position-sensitive fold: sampling two FIXED positions each iteration
         * captures the sequence of values that rotate through them, so a wrong
         * move ORDER (not just a wrong final multiset) changes the checksum —
         * even though each rotation alone is multiset-invariant. This also makes
         * the rotations observable, so the optimiser cannot delete them. */
        acc = (acc ^ a0) * 16777619u;
        acc = (acc ^ b7) * 16777619u;
        acc = (acc << 1) | (acc >> 31);
    }

    unsigned sum = a0+a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14
                 + b0+b1+b2+b3+b4+b5+b6+b7+b8+b9+b10+b11+b12+b13+b14;
    return (int)(acc ^ sum);
}
