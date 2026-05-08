#include "cvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Instruction encoders ------------------------------------------------ */

static uint32_t enc_r(uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)b << 16)
         | ((uint32_t)c << 24);
}
static uint32_t enc_i16(uint8_t op, uint8_t a, int16_t imm) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)(uint16_t)imm << 16);
}
static uint32_t enc_i24(uint8_t op, int32_t imm) {
    return (uint32_t)op | (((uint32_t)imm & 0xFFFFFFu) << 8);
}
static uint32_t enc_br(uint8_t op, uint8_t a, uint8_t b, int8_t off) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)b << 16)
         | ((uint32_t)(uint8_t)off << 24);
}

/* --- Build a binary blob in memory --------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t *build_blob(const uint32_t *code, uint32_t code_count,
                           uint32_t bss_size, uint32_t entry,
                           size_t *out_len)
{
    uint32_t section_count = 1 + (bss_size > 0 ? 1 : 0);
    uint32_t header_size   = 24;
    uint32_t section_size  = 16;
    uint32_t table_off     = header_size;
    uint32_t code_off      = table_off + section_count * section_size;
    uint32_t code_size     = code_count * 4u;
    size_t   total         = (size_t)code_off + code_size;

    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) { fprintf(stderr, "calloc failed\n"); exit(1); }

    buf[0] = 'C'; buf[1] = 'V'; buf[2] = 'M'; buf[3] = '1';
    put_u32(buf + 4,  0x00010000u);    /* version */
    put_u32(buf + 8,  0u);             /* flags */
    put_u32(buf + 12, section_count);
    put_u32(buf + 16, table_off);
    put_u32(buf + 20, entry);

    /* CODE section */
    put_u32(buf + table_off + 0,  CVM_SEC_CODE);
    put_u32(buf + table_off + 4,  code_off);
    put_u32(buf + table_off + 8,  code_size);
    put_u32(buf + table_off + 12, 0u);

    if (bss_size > 0) {
        uint32_t bss_entry = table_off + section_size;
        put_u32(buf + bss_entry + 0,  CVM_SEC_BSS);
        put_u32(buf + bss_entry + 4,  0u);
        put_u32(buf + bss_entry + 8,  bss_size);
        put_u32(buf + bss_entry + 12, 0u);
    }

    for (uint32_t i = 0; i < code_count; ++i)
        put_u32(buf + code_off + i * 4u, code[i]);

    *out_len = total;
    return buf;
}

/* --- Test harness -------------------------------------------------------- */

static int g_failures = 0;

#define CHECK(cond, ...) do {                                  \
    if (!(cond)) {                                             \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
        fprintf(stderr, __VA_ARGS__);                          \
        fprintf(stderr, "\n");                                 \
        g_failures++;                                          \
    }                                                          \
} while (0)

static int run_image(const uint32_t *code, uint32_t n,
                     uint32_t bss_size, uint32_t entry,
                     int32_t *out)
{
    size_t len;
    uint8_t *blob = build_blob(code, n, bss_size, entry, &len);
    struct cvm_image img;
    int r = cvm_load(blob, len, &img);
    if (r == CVM_OK) {
        r = cvm_run(&img, out);
        cvm_image_free(&img);
    }
    free(blob);
    return r;
}

/* --- Cases --------------------------------------------------------------- */

static void test_add(void) {
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 7),
        enc_i16(CVM_OP_MOVI, 1, 5),
        enc_r  (CVM_OP_ADD,  0, 0, 1),
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 4, 0, 0, &v);
    CHECK(r == CVM_OK, "add: run %s", cvm_strerror(r));
    CHECK(v == 12,     "add: got %d", v);
}

static void test_arith(void) {
    /* (10 - 3) * 6 = 42 */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 10),
        enc_i16(CVM_OP_MOVI, 1, 3),
        enc_r  (CVM_OP_SUB,  2, 0, 1),
        enc_i16(CVM_OP_MOVI, 3, 6),
        enc_r  (CVM_OP_MUL,  4, 2, 3),
        enc_r  (CVM_OP_HALT, 4, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 6, 0, 0, &v);
    CHECK(r == CVM_OK, "arith: %s", cvm_strerror(r));
    CHECK(v == 42,     "arith: got %d", v);
}

static void test_mem_roundtrip(void) {
    /* heap[4] = 0xCAFE; r0 = *(i32*)&heap[4] */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 4),       /* address */
        enc_i16(CVM_OP_MOVI, 1, 0x4afe),  /* value (positive 16-bit) */
        enc_r  (CVM_OP_STW,  0, 0, 1),    /* heap[R0] = R1 */
        enc_r  (CVM_OP_LDW,  2, 0, 0),    /* R2 = heap[R0] */
        enc_r  (CVM_OP_HALT, 2, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 5, 16, 0, &v);
    CHECK(r == CVM_OK,  "mem: %s", cvm_strerror(r));
    CHECK(v == 0x4afe,  "mem: got 0x%x", (unsigned)v);
}

static void test_loop_sum(void) {
    /* sum = 1+2+...+10 = 55 */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 0),     /* 0: sum   = 0 */
        enc_i16(CVM_OP_MOVI, 1, 1),     /* 1: i     = 1 */
        enc_i16(CVM_OP_MOVI, 2, 11),    /* 2: limit = 11 */
        enc_r  (CVM_OP_ADD,  0, 0, 1),  /* 3: sum  += i        <-- loop */
        enc_i16(CVM_OP_MOVI, 3, 1),     /* 4: one   = 1 */
        enc_r  (CVM_OP_ADD,  1, 1, 3),  /* 5: i    += one */
        enc_br (CVM_OP_BNE,  1, 2, -4), /* 6: if i != limit, pc = 7 + (-4) = 3 */
        enc_r  (CVM_OP_HALT, 0, 0, 0),  /* 7 */
    };
    int32_t v = 0;
    int r = run_image(code, 8, 0, 0, &v);
    CHECK(r == CVM_OK, "loop: %s", cvm_strerror(r));
    CHECK(v == 55,     "loop: got %d", v);
}

static void test_beq_taken(void) {
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 5),
        enc_i16(CVM_OP_MOVI, 1, 5),
        enc_br (CVM_OP_BEQ,  0, 1, 1),     /* skip next */
        enc_i16(CVM_OP_MOVI, 0, 99),       /* skipped */
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 5, 0, 0, &v);
    CHECK(r == CVM_OK, "beq taken: %s", cvm_strerror(r));
    CHECK(v == 5,      "beq taken: got %d", v);
}

static void test_beq_not_taken(void) {
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 5),
        enc_i16(CVM_OP_MOVI, 1, 6),
        enc_br (CVM_OP_BEQ,  0, 1, 1),
        enc_i16(CVM_OP_MOVI, 0, 99),
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 5, 0, 0, &v);
    CHECK(r == CVM_OK, "beq nt: %s", cvm_strerror(r));
    CHECK(v == 99,     "beq nt: got %d", v);
}

static void test_jmp(void) {
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 1),
        enc_i24(CVM_OP_JMP,  2),           /* skip next two */
        enc_i16(CVM_OP_MOVI, 0, 99),
        enc_i16(CVM_OP_MOVI, 0, 50),
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 5, 0, 0, &v);
    CHECK(r == CVM_OK, "jmp: %s", cvm_strerror(r));
    CHECK(v == 1,      "jmp: got %d", v);
}

static void test_bad_addr(void) {
    /* Try to load from heap address 0 with no heap. */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 0),
        enc_r  (CVM_OP_LDW,  1, 0, 0),
        enc_r  (CVM_OP_HALT, 1, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 3, 0, 0, &v);
    CHECK(r == CVM_E_BAD_ADDR, "bad addr: got %s", cvm_strerror(r));
}

static void test_loader_rejects(void) {
    /* Bad magic */
    uint8_t bogus[24] = {0};
    struct cvm_image img;
    int r = cvm_load(bogus, sizeof(bogus), &img);
    CHECK(r == CVM_E_BAD_MAGIC, "loader magic: got %s", cvm_strerror(r));

    /* Truncated */
    r = cvm_load("CVM1", 4, &img);
    CHECK(r == CVM_E_TRUNCATED, "loader trunc: got %s", cvm_strerror(r));
}

int main(void) {
    test_add();
    test_arith();
    test_mem_roundtrip();
    test_loop_sum();
    test_beq_taken();
    test_beq_not_taken();
    test_jmp();
    test_bad_addr();
    test_loader_rejects();

    if (g_failures) {
        fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
