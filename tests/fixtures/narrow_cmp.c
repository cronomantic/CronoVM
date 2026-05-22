/* Narrow-width integer comparisons (icmp on i8/i16). The VM has only 32-bit
 * compares, so both icmp operands must be normalised to the operand width
 * first — loads zero-extend (LDB/LDH) but constants are materialised
 * sign-extended, so a raw 32-bit compare of `iN` values disagrees in the high
 * bits. Regression test for the `icmp eq i8 %v, -1` infinite loop (a
 * zero-extended loaded 0x000000FF never equalled a sign-extended 0xFFFFFFFF)
 * that hung Crispy Doom's P_InitPicAnims animdef terminator scan.
 *
 * Globals (not const, loaded at runtime so nothing folds away):
 *   tags[]  : signed-char table terminated by -1, walked with `!= -1`.
 *   svals[] : signed chars tested `< 0`  → SIGNED compare (sign-extend path).
 *   uvals[] : unsigned chars tested `> 100` → UNSIGNED compare (zero-extend).
 *
 * narrow_cmp_main(4):
 *   terminator scan stops at tags[2] == -1            → i      =    2
 *   svals < 0 : -5,-1 hit (3 misses)         → +100 each →     +  200
 *   uvals > 100: 200,255 hit (0 misses)      → +1000 each →    + 2000
 *   total                                                      =  2202
 * With the bug the scan never sees the -1 terminator (runs to n=4) and the
 * signed `< 0` compares all read zero-extended (never negative), so the buggy
 * result is 4 + 0 + 2000 = 2004 — a clean mismatch.
 */
signed   char tags[4]  = {0, 1, -1, 7};
signed   char svals[3] = {-5, -1, 3};
unsigned char uvals[3] = {200, 0, 255};

int narrow_cmp_main(int n)
{
    int i = 0;
    while (i < n && tags[i] != -1) i++;     /* icmp eq i8, -1 : the bug */
    int r = i;

    for (int k = 0; k < 3; k++)
        if (svals[k] < 0)  r += 100;        /* icmp slt i8, 0 : sign-extend */

    for (int k = 0; k < 3; k++)
        if (uvals[k] > 100) r += 1000;      /* icmp ugt i8, 100 : zero-extend */

    return r;
}
