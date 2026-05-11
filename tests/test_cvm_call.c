/* test_cvm_call — exercises cvm_call (re-enter VM at a FUNCS index).
 *
 * The Cronopio host calls this every frame to invoke the cart-registered
 * frame fn. The contract we need to lock in:
 *   1. cvm_call(img, idx, args, n, &ret) seeds R0..Rn-1 with `args`, runs
 *      from FUNCS[idx], and on RET-against-sentinel returns R[0] in *ret.
 *   2. The heap persists across calls (so consecutive frames see the same
 *      globals); the register file is fresh each call.
 *   3. Error cases trap correctly: idx==0 → CVM_E_NULL_FUNC_PTR,
 *      idx≥func_count → CVM_E_BAD_FUNC_INDEX, no FUNCS at all →
 *      CVM_E_BAD_FUNCS.
 *
 * Hand-assembled blobs only — keeps the test independent of the translator. */

#include "cvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...) do {                                  \
    if (!(cond)) {                                             \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
        fprintf(stderr, __VA_ARGS__);                          \
        fprintf(stderr, "\n");                                 \
        g_failures++;                                          \
    }                                                          \
} while (0)

/* --- encoders + blob builder --------------------------------------------- */

static uint32_t enc_r(uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    return (uint32_t)op | ((uint32_t)a << 8) | ((uint32_t)b << 16) | ((uint32_t)c << 24);
}
static uint32_t enc_i16(uint8_t op, uint8_t a, int16_t imm) {
    return (uint32_t)op | ((uint32_t)a << 8) | ((uint32_t)(uint16_t)imm << 16);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

/* Minimal blob builder: CODE + optional FUNCS + STACK_RESERVE. */
static uint8_t *build_blob(const uint32_t *code, uint32_t code_count,
                           uint32_t entry,
                           const uint32_t *funcs, uint32_t func_count,
                           uint32_t stack_size,
                           size_t *out_len)
{
    uint32_t header   = 24;
    uint32_t sec_size = 16;
    uint32_t sec_count = 1
                       + (func_count > 0 ? 1 : 0)
                       + (stack_size > 0 ? 1 : 0);
    uint32_t table_off = header;
    uint32_t cursor    = table_off + sec_count * sec_size;
    uint32_t code_off  = cursor;
    uint32_t code_size = code_count * 4u;
    cursor += code_size;
    uint32_t funcs_off = func_count > 0 ? cursor : 0;
    uint32_t funcs_size = func_count * 4u;
    cursor += funcs_size;
    size_t total = cursor;

    uint8_t *buf = (uint8_t*)calloc(1, total);
    buf[0]='C'; buf[1]='V'; buf[2]='M'; buf[3]='1';
    put_u32(buf+4, 0x00010000u);
    put_u32(buf+8, 0);
    put_u32(buf+12, sec_count);
    put_u32(buf+16, table_off);
    put_u32(buf+20, entry);

    uint32_t te = table_off;
    put_u32(buf+te+0, CVM_SEC_CODE);
    put_u32(buf+te+4, code_off);
    put_u32(buf+te+8, code_size);
    put_u32(buf+te+12, 0);
    te += sec_size;
    if (funcs_size > 0) {
        put_u32(buf+te+0, CVM_SEC_FUNCS);
        put_u32(buf+te+4, funcs_off);
        put_u32(buf+te+8, funcs_size);
        put_u32(buf+te+12, 0);
        te += sec_size;
    }
    if (stack_size > 0) {
        put_u32(buf+te+0, CVM_SEC_STACK_RESERVE);
        put_u32(buf+te+4, 0);
        put_u32(buf+te+8, stack_size);
        put_u32(buf+te+12, 0);
        te += sec_size;
    }

    for (uint32_t i = 0; i < code_count; ++i)
        put_u32(buf+code_off + i*4u, code[i]);
    for (uint32_t i = 0; i < func_count; ++i)
        put_u32(buf+funcs_off + i*4u, funcs[i]);

    *out_len = total;
    return buf;
}

/* --- tests --------------------------------------------------------------- */

/* Layout:
 *   pc=0  entry: MOVI R0, 7; HALT R0                 (returns 7 normally)
 *   pc=2  funcA: MOVI R0, 42; RET                    (FUNCS[1])
 *   pc=4  funcB: ADD R0, R0, R1; RET                 (FUNCS[2]) — adds R0+R1
 */
static void test_call_basic(void) {
    uint32_t code[] = {
        /* 0 entry */ enc_i16(CVM_OP_MOVI, 0, 7),
        /* 1       */ enc_r  (CVM_OP_HALT, 0, 0, 0),
        /* 2 funcA */ enc_i16(CVM_OP_MOVI, 0, 42),
        /* 3       */ enc_r  (CVM_OP_RET,  0, 0, 0),
        /* 4 funcB */ enc_r  (CVM_OP_ADD,  0, 0, 1),
        /* 5       */ enc_r  (CVM_OP_RET,  0, 0, 0),
    };
    uint32_t funcs[] = { 0u /* reserved */, 2u /* funcA */, 4u /* funcB */ };
    size_t   len;
    uint8_t *blob = build_blob(code, 6, /*entry*/0, funcs, 3, /*stack*/64, &len);

    struct cvm_image img;
    int rc = cvm_load(blob, len, &img);
    CHECK(rc == CVM_OK, "load: %s", cvm_strerror(rc));

    /* Run the entry first — establishes that we can still cvm_run a cart
     * with FUNCS. */
    int32_t v = 0;
    rc = cvm_run(&img, &v);
    CHECK(rc == CVM_OK && v == 7, "entry: rc=%s v=%d", cvm_strerror(rc), v);

    /* Call FUNCS[1] with no args — expect 42. */
    v = 0;
    rc = cvm_call(&img, 1, NULL, 0, &v);
    CHECK(rc == CVM_OK && v == 42, "funcA: rc=%s v=%d", cvm_strerror(rc), v);

    /* Call FUNCS[2] with two args — expect 3+4=7. */
    int32_t args[2] = { 3, 4 };
    v = 0;
    rc = cvm_call(&img, 2, args, 2, &v);
    CHECK(rc == CVM_OK && v == 7, "funcB: rc=%s v=%d", cvm_strerror(rc), v);

    /* Call FUNCS[2] again with different args — fresh register file each
     * call (no leak from previous). */
    args[0] = 100; args[1] = 23;
    v = 0;
    rc = cvm_call(&img, 2, args, 2, &v);
    CHECK(rc == CVM_OK && v == 123, "funcB second: rc=%s v=%d", cvm_strerror(rc), v);

    cvm_image_free(&img); free(blob);
}

static void test_call_errors(void) {
    /* Image with FUNCS = { 0, pc=0 }. Entry is a simple HALT. */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 0),
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    uint32_t funcs[] = { 0u, 0u };
    size_t   len;
    uint8_t *blob = build_blob(code, 2, /*entry*/0, funcs, 2, /*stack*/64, &len);

    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "errors: load");

    int32_t v = 0;
    int rc;
    rc = cvm_call(&img, 0, NULL, 0, &v);
    CHECK(rc == CVM_E_NULL_FUNC_PTR, "idx0: got %s", cvm_strerror(rc));
    rc = cvm_call(&img, 999, NULL, 0, &v);
    CHECK(rc == CVM_E_BAD_FUNC_INDEX, "idx999: got %s", cvm_strerror(rc));

    cvm_image_free(&img); free(blob);

    /* Image with no FUNCS section — cvm_call must refuse. */
    uint8_t *blob2 = build_blob(code, 2, 0, NULL, 0, 0, &len);
    struct cvm_image img2;
    CHECK(cvm_load(blob2, len, &img2) == CVM_OK, "errors: load2");
    rc = cvm_call(&img2, 1, NULL, 0, &v);
    CHECK(rc == CVM_E_BAD_FUNCS, "no funcs: got %s", cvm_strerror(rc));
    cvm_image_free(&img2); free(blob2);
}

int main(void) {
    test_call_basic();
    test_call_errors();
    if (g_failures > 0) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    fprintf(stderr, "test_cvm_call: ok\n");
    return 0;
}
