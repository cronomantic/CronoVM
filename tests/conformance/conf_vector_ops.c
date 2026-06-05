/* conf_vector_ops.c — conformance slice: fixed-vector load/store + arithmetic.
 *
 * The translator legalises fixed integer/pointer vectors by scalarisation (one
 * 32-bit frame slot per lane). Originally only libc++'s num_get movemask idiom
 * was lowered (insertelement / shufflevector / vector-icmp / sext|zext|trunc).
 * clang's -O2+ auto-vectoriser also emits, for ordinary code, a vector
 * MEMORY-COPY (`load`/`store <N x iM>` — e.g. a `<2 x ptr>` struct/union copy,
 * the Exult Usecode_value case) and vector ARITHMETIC (the f64 soft-float
 * runtime emits `<N x i32> xor`). Those are now lowered per-lane.
 *
 * This fixture forces exactly those ops at ANY -O level using clang's
 * `vector_size` extension (the runner builds at -O1, where struct-copy
 * auto-vectorisation does NOT fire — so explicit vector types are needed to
 * exercise the new paths deterministically). It round-trips vectors through
 * VOLATILE memory (forcing real vector load + store) and folds the results into
 * an int32 checksum compared against the native oracle.
 *
 * UB discipline (corpus rule): arithmetic uses UNSIGNED lanes (two's-complement
 * wrap is defined; signed overflow is UB and would diverge host-vs-VM), and
 * every shift amount is < the element width. One signed vector exercises ashr
 * (arithmetic right shift) with safe shift counts. */
#include <stdint.h>

typedef uint32_t v4u  __attribute__((vector_size(16)));   /* <4 x i32> */
typedef int32_t  v4i  __attribute__((vector_size(16)));   /* <4 x i32> (signed, ashr) */
typedef uint16_t v8h  __attribute__((vector_size(16)));   /* <8 x i16> */
typedef uint8_t  v16b __attribute__((vector_size(16)));   /* <16 x i8> */

int conf_main(void) {
    int32_t acc = 0;

    /* --- <4 x i32> unsigned: load (from volatile mem) + each arithmetic/bitwise
     *     op, stored back to volatile mem (load + store + binop per op). --- */
    volatile uint32_t srcA[4] = { 0x12345678u, 0xFFFFFF00u, 1000000u, 0x0000ABCDu };
    volatile uint32_t srcB[4] = { 0x000000FFu, 13u, 7u, 0x00FF00FFu };
    volatile uint32_t sh[4]   = { 1u, 4u, 8u, 31u };   /* all < 32 */
    v4u a = *(const volatile v4u *)srcA;   /* vector load */
    v4u b = *(const volatile v4u *)srcB;   /* vector load */
    v4u s = *(const volatile v4u *)sh;

    volatile uint32_t out[4];
    *(volatile v4u *)out = a + b;  for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* add */
    *(volatile v4u *)out = a - b;  for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* sub */
    *(volatile v4u *)out = a * b;  for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* mul */
    *(volatile v4u *)out = a & b;  for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* and */
    *(volatile v4u *)out = a | b;  for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* or  */
    *(volatile v4u *)out = a ^ b;  for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* xor */
    *(volatile v4u *)out = a << s; for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* shl */
    *(volatile v4u *)out = a >> s; for (int k=0;k<4;++k) acc = acc*31 + (int32_t)out[k];  /* lshr (unsigned) */

    /* signed arithmetic right shift (ashr), safe shift counts < 32 */
    volatile int32_t srcS[4] = { -2000000000, -1, 0x40000000, -12345 };
    volatile int32_t shS[4]  = { 1, 4, 8, 15 };
    v4i sa = *(const volatile v4i *)srcS;
    v4i sb = *(const volatile v4i *)shS;
    volatile int32_t outS[4];
    *(volatile v4i *)outS = sa >> sb;  for (int k=0;k<4;++k) acc = acc*31 + outS[k];   /* ashr */

    /* --- narrow lanes: <8 x i16> and <16 x i8> arithmetic round-trips, to
     *     exercise sub-word element load/store strides (LDH/STH, LDB/STB). --- */
    volatile uint16_t hA[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    volatile uint16_t hB[8] = { 100, 200, 300, 400, 500, 600, 700, 800 };
    v8h ha = *(const volatile v8h *)hA;
    v8h hb = *(const volatile v8h *)hB;
    volatile uint16_t hout[8];
    *(volatile v8h *)hout = ha + hb;  for (int k=0;k<8;++k) acc = acc*31 + (int32_t)hout[k];
    *(volatile v8h *)hout = ha ^ hb;  for (int k=0;k<8;++k) acc = acc*31 + (int32_t)hout[k];

    volatile uint8_t bA[16];
    volatile uint8_t bB[16];
    for (int k = 0; k < 16; ++k) { bA[k] = (uint8_t)(k * 7 + 5); bB[k] = (uint8_t)(k * 3 + 1); }
    v16b ba = *(const volatile v16b *)bA;
    v16b bb = *(const volatile v16b *)bB;
    volatile uint8_t bout[16];
    *(volatile v16b *)bout = ba - bb;  for (int k=0;k<16;++k) acc = acc*31 + (int32_t)bout[k];
    *(volatile v16b *)bout = ba & bb;  for (int k=0;k<16;++k) acc = acc*31 + (int32_t)bout[k];

    return acc;
}
