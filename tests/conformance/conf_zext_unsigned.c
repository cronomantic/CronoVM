/* conf_zext_unsigned.c — guards the "ZExt must mask to source width" fix.
 *
 * A narrow UNSIGNED value whose high bit is set (> 127 for u8, > 32767 for u16)
 * must ZERO-extend, not sign-extend, when widened to int. The translator bug:
 * narrow integer CONSTANTS are materialised sign-extended
 * (LLVMConstIntGetSExtValue), and a function argument of unsigned-narrow type
 * carries that sign-extended bit pattern into the callee, where a bare-MOV ZExt
 * then yields a negative value — e.g. (unsigned char)160 -> 0xFFFFFFA0 == -96.
 * Real-world bite: UQM's interplanetary music was silent because
 * FadeMusic(NORMAL_VOLUME == 160) arrived as -96, clamping the volume to 0.
 *
 * The existing conf_int_width covers sext/zext round-trips but with values that
 * don't isolate the sign-extended-constant-through-an-unsigned-param path. This
 * fixture pins it. Differential vs the native oracle; int32 checksum.
 */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* unsigned-narrow PARAMETER widened to int (the FadeMusic(BYTE) pattern): the
 * caller materialises the i8/i16 constant, the callee ZExts it to i32. */
static int NOINLINE wid_u8 (uint8_t  x) { return (int)x; }
static int NOINLINE wid_u16(uint16_t x) { return (int)x; }
static int NOINLINE wid_s8 (int8_t   x) { return (int)x; }  /* signed stays signed */
static int NOINLINE wid_s16(int16_t  x) { return (int)x; }

int32_t conf_main(void) {
    int32_t cs = 0;

    /* Constants with the high bit set, passed through narrow UNSIGNED params. */
    cs += wid_u8(160);          /* 160, not -96 */
    cs += wid_u8(255) * 3;      /* 255, not -1  */
    cs += wid_u8(128) * 5;      /* 128, not -128 */
    cs += wid_u16(40000);       /* 40000, not -25536 */
    cs += wid_u16(0x8000u) * 2; /* 32768 */

    /* Signed narrow params must KEEP their sign (the fix must not break SExt). */
    cs += wid_s8(-96) * 7;
    cs += wid_s8(-1) * 11;
    cs += wid_s16(-30000) * 13;

    /* Runtime (non-constant) narrow unsigned values widened to int. */
    for (unsigned v = 120; v < 270; v += 13) {
        uint8_t  b = (uint8_t)v;            /* trunc i32 -> u8  */
        uint16_t h = (uint16_t)(v * 521u);  /* trunc i32 -> u16 */
        cs += (int)b * 3;                   /* zext  u8  -> i32 */
        cs += (int)h;                       /* zext  u16 -> i32 */
        cs += wid_u8((uint8_t)(v + 7));     /* same, via an unsigned param */
    }
    return cs;
}
