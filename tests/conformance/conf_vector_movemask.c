/* conf_vector_movemask.c — conformance slice: the fixed-width VECTOR MOVEMASK
 * idiom that libc++'s std::num_get emits when it parses a number from a stream.
 *
 * char_traits<char>::find over the 32 digit-atoms "0123456789abcdefABCDEFxX+-.."
 * is lowered (even with NO SIMD target, because libc++ uses explicit vector
 * builtins, not auto-vectorisation) to a SPLAT + a vector compare + a MOVEMASK:
 *
 *     %s  = insertelement <32 x i8> poison, i8 %needle, i64 0
 *     %v  = shufflevector  <32 x i8> %s, poison, <32 x i32> zeroinitializer  ; splat
 *     %c  = icmp eq <32 x i8> %v, <i8 48, i8 49, ...>      ; -> <32 x i1>
 *     %m  = bitcast <32 x i1> %c to i32                    ; the movemask
 *     ... __countr_zero(%m)  -> the matching lane index
 *
 * The translator legalises a <N x iM> vector value as N consecutive 32-bit frame
 * slots (one lane per slot, like the i64 2-slot / i65 3-slot wide values) and
 * lowers each vector op to per-lane scalar VM ops; `bitcast <N x i1> to iN` packs
 * the lane booleans into an integer (lane k -> bit k). All inputs are volatile so
 * nothing folds; fixed-width + int32 checksum, so host-64 and VM-32 agree
 * bit-for-bit. Differential vs the native oracle.
 *
 * Reproduces the EXACT -O1 libc++ shape (verified against the vendored
 * locale.cpp): insertelement / shufflevector-splat / icmp eq+ne <Nxi8> /
 * bitcast <Nxi1>->iN. A 16-lane variant proves the lowering is width-generic. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

typedef int8_t  v32i8 __attribute__((vector_size(32)));
typedef int8_t  v16i8 __attribute__((vector_size(16)));
typedef _Bool   vb32  __attribute__((ext_vector_type(32)));
typedef _Bool   vb16  __attribute__((ext_vector_type(16)));

/* The 32 digit-atoms num_get scans, exactly as in libc++ num.h. */
static const v32i8 ATOMS = { 48,49,50,51,52,53,54,55,56,57,        /* 0-9 */
                             97,98,99,100,101,102,                  /* a-f */
                             65,66,67,68,69,70,                     /* A-F */
                             120,88,43,45,46,101,112,80,111,79 };   /* xX+-.epPoO */

/* movemask of (splat(needle) == ATOMS): bit k set iff lane k matched. The exact
 * insertelement/shufflevector(splat)/icmp-eq/bitcast<32xi1>->i32 idiom. */
static uint32_t NOINLINE eqmask32(int8_t needle) {
    v32i8 splat = needle - (v32i8){0};                 /* broadcast lane 0 */
    vb32 m = __builtin_convertvector(splat == ATOMS, vb32);
    uint32_t mask; __builtin_memcpy(&mask, &m, sizeof mask);
    return mask;
}

/* char_traits::find: first matching atom index, or -1 (movemask + cttz). */
static int NOINLINE find_atom(int8_t needle) {
    uint32_t mask = eqmask32(needle);
    return mask ? __builtin_ctz(mask) : -1;
}

/* icmp NE variant + popcount: how many lanes DIFFER from the splat. Exercises
 * `icmp ne <32 x i8>` -> bitcast (num_get also forms the ne-vs-zero mask). */
static int NOINLINE count_ne32(int8_t needle) {
    v32i8 splat = needle - (v32i8){0};
    vb32 m = __builtin_convertvector(splat != ATOMS, vb32);
    uint32_t mask; __builtin_memcpy(&mask, &m, sizeof mask);
    return __builtin_popcount(mask);
}

/* 16-lane variant: proves the N-slot lowering is width-generic (not hardcoded to
 * 32). splat(needle) == const16, bitcast <16 x i1> -> i16 zero-extended. The
 * needle is the only runtime input, so the table stays a constant operand (no
 * vector load) — exactly the real -O1 cxxio.bc surface. */
static const v16i8 HEX16 = { 48,49,50,51,52,53,54,55,56,57,97,98,99,100,101,102 };
static uint32_t NOINLINE eqmask16(int8_t needle) {
    v16i8 splat = needle - (v16i8){0};
    vb16 m = __builtin_convertvector(splat == HEX16, vb16);
    uint16_t mask; __builtin_memcpy(&mask, &m, sizeof mask);
    return (uint32_t)mask;
}

static volatile int8_t needles[12] =
    { '0','9','a','F','x','+','.','z','A','5','-','Q' };

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int i = 0; i < 12; ++i) {
        int8_t n = needles[i];
        MIX(eqmask32(n));
        MIX(find_atom(n));
        MIX(count_ne32(n));
        MIX(eqmask16(n));
    }
    /* sweep the full byte range through find_atom so every lane boundary and the
     * not-found path are hit. */
    for (int c = -128; c < 128; ++c)
        MIX(find_atom((int8_t)c));

    #undef MIX
    return (int)h;
}
