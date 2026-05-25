/* Regression: a value used in MULTIPLE operand slots of ONE instruction
 * (here v[i]*v[i] -> llvm.fmuladd.f32(v[i], v[i], acc)) must be returned to the
 * register pool only ONCE. The per-operand free loop reaches it once per
 * occurrence; without dedup in cg_free_reg its register was pooled twice, and
 * two later defs both drew the SAME register. In this counted loop the second
 * def (the exit `icmp`) then overwrote the counter increment %8 — which is also
 * the back-edge phi source — so the counter never advanced and the loop spun
 * forever. (Surfaced by Quake's Length() in Mod_LoadTexinfo.)
 *
 * vm_main(n) sums the squares of {n, 2, 3} = n*n + 13.  n=4 -> 29 ; n=0 -> 13. */

typedef float vec3_t[3];

__attribute__((noinline))
static float Length2(vec3_t v) {
    int   i;
    float s = 0;
    for (i = 0; i < 3; i++)
        s += v[i] * v[i];
    return s;
}

int vm_main(int n) {
    vec3_t v;
    v[0] = (float)n;
    v[1] = 2.0f;
    v[2] = 3.0f;
    return (int)Length2(v);
}
