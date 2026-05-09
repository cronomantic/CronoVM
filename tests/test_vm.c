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
static uint32_t enc_syscall(uint16_t id) {
    return (uint32_t)CVM_OP_SYSCALL | ((uint32_t)id << 16);
}

/* --- Build a binary blob in memory --------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t *build_blob_full(const uint32_t *code, uint32_t code_count,
                                uint32_t bss_size, uint32_t entry,
                                const char * const *imports, uint32_t import_count,
                                const uint8_t *data, uint32_t data_size,
                                uint32_t reserve_size,
                                const uint32_t *funcs, uint32_t func_count,
                                uint32_t stack_size,
                                size_t *out_len)
{
    uint32_t header_size  = 24;
    uint32_t section_size = 16;

    /* Compute IMPORTS section size up front. */
    uint32_t imports_size = 0;
    if (import_count > 0) {
        imports_size = 4u + import_count * 4u;
        for (uint32_t i = 0; i < import_count; ++i)
            imports_size += (uint32_t)strlen(imports[i]) + 1u;
    }
    uint32_t funcs_size = func_count > 0 ? func_count * 4u : 0;

    uint32_t section_count =
          1u                                /* CODE always */
        + (bss_size > 0 ? 1u : 0u)
        + (import_count > 0 ? 1u : 0u)
        + (data_size > 0 ? 1u : 0u)
        + (reserve_size > 0 ? 1u : 0u)
        + (stack_size > 0 ? 1u : 0u)
        + (funcs_size > 0 ? 1u : 0u);

    uint32_t table_off = header_size;
    uint32_t cursor    = table_off + section_count * section_size;
    uint32_t code_off  = cursor;
    uint32_t code_size = code_count * 4u;
    cursor += code_size;
    uint32_t data_off = data_size > 0 ? cursor : 0;
    cursor += data_size;
    uint32_t imports_off = imports_size > 0 ? cursor : 0;
    cursor += imports_size;
    uint32_t funcs_off = funcs_size > 0 ? cursor : 0;
    cursor += funcs_size;
    size_t total = cursor;

    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) { fprintf(stderr, "calloc failed\n"); exit(1); }

    buf[0] = 'C'; buf[1] = 'V'; buf[2] = 'M'; buf[3] = '1';
    put_u32(buf + 4,  0x00010000u);
    put_u32(buf + 8,  0u);
    put_u32(buf + 12, section_count);
    put_u32(buf + 16, table_off);
    put_u32(buf + 20, entry);

    uint32_t te = table_off;
    put_u32(buf + te + 0, CVM_SEC_CODE);
    put_u32(buf + te + 4, code_off);
    put_u32(buf + te + 8, code_size);
    put_u32(buf + te + 12, 0u);
    te += section_size;

    if (data_size > 0) {
        put_u32(buf + te + 0, CVM_SEC_DATA);
        put_u32(buf + te + 4, data_off);
        put_u32(buf + te + 8, data_size);
        put_u32(buf + te + 12, 0u);
        te += section_size;
    }
    if (bss_size > 0) {
        put_u32(buf + te + 0, CVM_SEC_BSS);
        put_u32(buf + te + 4, 0u);
        put_u32(buf + te + 8, bss_size);
        put_u32(buf + te + 12, 0u);
        te += section_size;
    }
    if (import_count > 0) {
        put_u32(buf + te + 0, CVM_SEC_IMPORTS);
        put_u32(buf + te + 4, imports_off);
        put_u32(buf + te + 8, imports_size);
        put_u32(buf + te + 12, 0u);
        te += section_size;
    }
    if (reserve_size > 0) {
        put_u32(buf + te + 0, CVM_SEC_HEAP_RESERVE);
        put_u32(buf + te + 4, 0u);
        put_u32(buf + te + 8, reserve_size);
        put_u32(buf + te + 12, 0u);
        te += section_size;
    }
    if (stack_size > 0) {
        put_u32(buf + te + 0, CVM_SEC_STACK_RESERVE);
        put_u32(buf + te + 4, 0u);
        put_u32(buf + te + 8, stack_size);
        put_u32(buf + te + 12, 0u);
        te += section_size;
    }
    if (funcs_size > 0) {
        put_u32(buf + te + 0, CVM_SEC_FUNCS);
        put_u32(buf + te + 4, funcs_off);
        put_u32(buf + te + 8, funcs_size);
        put_u32(buf + te + 12, 0u);
        te += section_size;
    }

    for (uint32_t i = 0; i < code_count; ++i)
        put_u32(buf + code_off + i * 4u, code[i]);

    if (data_size > 0)
        memcpy(buf + data_off, data, data_size);

    if (funcs_size > 0) {
        for (uint32_t i = 0; i < func_count; ++i)
            put_u32(buf + funcs_off + i * 4u, funcs[i]);
    }

    if (import_count > 0) {
        uint8_t *p = buf + imports_off;
        put_u32(p, import_count);
        uint32_t name_cursor = 4u + import_count * 4u;
        for (uint32_t i = 0; i < import_count; ++i) {
            put_u32(p + 4u + i * 4u, name_cursor);
            size_t L = strlen(imports[i]);
            memcpy(p + name_cursor, imports[i], L + 1);
            name_cursor += (uint32_t)L + 1u;
        }
    }

    *out_len = total;
    return buf;
}

static uint8_t *build_blob(const uint32_t *code, uint32_t code_count,
                           uint32_t bss_size, uint32_t entry,
                           size_t *out_len)
{
    return build_blob_full(code, code_count, bss_size, entry,
                           NULL, 0, NULL, 0, 0,
                           NULL, 0, 0, out_len);
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

static void test_cmp_signed(void) {
    /* (-3 <  5) = 1, (-3 == -3) = 1, (-3 != 5) = 1, (-3 <= -3) = 1 -> sum = 4 */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, -3),
        enc_i16(CVM_OP_MOVI, 1, 5),
        enc_r  (CVM_OP_CMP_LT, 2, 0, 1),  /* R2 = 1 */
        enc_r  (CVM_OP_CMP_EQ, 3, 0, 0),  /* R3 = 1 */
        enc_r  (CVM_OP_CMP_NE, 4, 0, 1),  /* R4 = 1 */
        enc_r  (CVM_OP_CMP_LE, 5, 0, 0),  /* R5 = 1 */
        enc_r  (CVM_OP_ADD,  2, 2, 3),
        enc_r  (CVM_OP_ADD,  2, 2, 4),
        enc_r  (CVM_OP_ADD,  2, 2, 5),
        enc_r  (CVM_OP_HALT, 2, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 10, 0, 0, &v);
    CHECK(r == CVM_OK, "cmp_s: %s", cvm_strerror(r));
    CHECK(v == 4,      "cmp_s: got %d", v);
}

static void test_cmp_unsigned(void) {
    /* As signed, -1 < 5; as unsigned, -1 (= 0xFFFFFFFF) > 5. */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, -1),
        enc_i16(CVM_OP_MOVI, 1, 5),
        enc_r  (CVM_OP_CMP_LT,  2, 0, 1),  /* signed: 1 */
        enc_r  (CVM_OP_CMP_LTU, 3, 0, 1),  /* unsigned: 0 */
        enc_r  (CVM_OP_ADD,     4, 2, 3),  /* expect 1 */
        enc_r  (CVM_OP_HALT,    4, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 6, 0, 0, &v);
    CHECK(r == CVM_OK, "cmp_u: %s", cvm_strerror(r));
    CHECK(v == 1,      "cmp_u: got %d", v);
}

static void test_div_signed(void) {
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, -17),
        enc_i16(CVM_OP_MOVI, 1, 5),
        enc_r  (CVM_OP_DIV, 2, 0, 1),     /* -17 / 5 = -3 (truncated) */
        enc_r  (CVM_OP_MOD, 3, 0, 1),     /* -17 % 5 = -2  (sign of dividend) */
        enc_r  (CVM_OP_ADD, 4, 2, 3),     /* -3 + -2 = -5 */
        enc_r  (CVM_OP_HALT, 4, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 6, 0, 0, &v);
    CHECK(r == CVM_OK, "div_s: %s", cvm_strerror(r));
    CHECK(v == -5,     "div_s: got %d", v);
}

static void test_div_by_zero(void) {
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 5),
        enc_i16(CVM_OP_MOVI, 1, 0),
        enc_r  (CVM_OP_DIV, 2, 0, 1),
        enc_r  (CVM_OP_HALT, 2, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 4, 0, 0, &v);
    CHECK(r == CVM_E_DIV_BY_ZERO, "div0: got %s", cvm_strerror(r));
}

static void test_shifts(void) {
    /* 16<<2 = 64 ; 64>>1 = 32 ; arithmetic right of -8 by 1 = -4 */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 16),
        enc_i16(CVM_OP_MOVI, 1, 2),
        enc_r  (CVM_OP_SHL, 2, 0, 1),     /* 64 */
        enc_i16(CVM_OP_MOVI, 3, 1),
        enc_r  (CVM_OP_SHR, 4, 2, 3),     /* 32 */
        enc_i16(CVM_OP_MOVI, 5, -8),
        enc_r  (CVM_OP_SAR, 6, 5, 3),     /* -4 */
        enc_r  (CVM_OP_ADD, 7, 4, 6),     /* 32 + -4 = 28 */
        enc_r  (CVM_OP_HALT, 7, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 9, 0, 0, &v);
    CHECK(r == CVM_OK, "shifts: %s", cvm_strerror(r));
    CHECK(v == 28,     "shifts: got %d", v);
}

static void test_bitwise(void) {
    /* 0xFF & 0x0F = 0x0F; 0xFF ^ 0x0F = 0xF0; 0x0F | 0xF0 = 0xFF -> 255 */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 0xFF),
        enc_i16(CVM_OP_MOVI, 1, 0x0F),
        enc_r  (CVM_OP_AND, 2, 0, 1),     /* 0x0F */
        enc_r  (CVM_OP_XOR, 3, 0, 1),     /* 0xF0 */
        enc_r  (CVM_OP_OR,  4, 2, 3),     /* 0xFF */
        enc_r  (CVM_OP_HALT, 4, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 6, 0, 0, &v);
    CHECK(r == CVM_OK, "bitwise: %s", cvm_strerror(r));
    CHECK(v == 0xFF,   "bitwise: got 0x%x", (unsigned)v);
}

/* --- Syscall tests ------------------------------------------------------- */

struct print_capture {
    int32_t value;
    int     calls;
};

static int sys_print_int(struct cvm_image *img, int32_t *regs, void *user_data) {
    (void)img;
    struct print_capture *cap = (struct print_capture *)user_data;
    cap->value = regs[0];
    cap->calls++;
    regs[0] = 0;  /* return 0 */
    return 0;
}

static int sys_add(struct cvm_image *img, int32_t *regs, void *user_data) {
    (void)img; (void)user_data;
    regs[0] = regs[0] + regs[1];
    return 0;
}

static int sys_print_string(struct cvm_image *img, int32_t *regs, void *user_data) {
    char *out = (char *)user_data;
    uint32_t addr = (uint32_t)regs[0];
    if (addr >= img->heap_size) return 1;
    char buf[64];
    size_t max = sizeof(buf) - 1;
    size_t avail = (size_t)(img->heap_size - addr);
    if (avail < max) max = avail;
    if (cvm_heap_read(img, addr, buf, max) != CVM_OK) return 1;
    size_t i = 0;
    for (; i < max && buf[i]; ++i) out[i] = buf[i];
    out[i] = 0;
    regs[0] = (int32_t)i;
    return 0;
}

static int sys_trap(struct cvm_image *img, int32_t *regs, void *user_data) {
    (void)img; (void)regs; (void)user_data;
    return 1;  /* trap */
}

static void test_syscall_hello_int(void) {
    /* Call cvm_sys_print_int(42), then HALT R0 (which the handler set to 0). */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 42),
        enc_syscall(0),
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    const char *imports[] = { "cvm_sys_print_int" };

    size_t len;
    uint8_t *blob = build_blob_full(code, 3, 0, 0, imports, 1, NULL, 0, 0,
                                    NULL, 0, 0, &len);
    struct cvm_image img;
    int r = cvm_load(blob, len, &img);
    CHECK(r == CVM_OK, "hello: load %s", cvm_strerror(r));

    struct print_capture cap = { 0, 0 };
    r = cvm_link(&img, "cvm_sys_print_int", sys_print_int, &cap);
    CHECK(r == CVM_OK, "hello: link %s", cvm_strerror(r));

    int32_t v = -1;
    r = cvm_run(&img, &v);
    CHECK(r == CVM_OK,    "hello: run %s", cvm_strerror(r));
    CHECK(cap.calls == 1, "hello: calls=%d", cap.calls);
    CHECK(cap.value == 42,"hello: value=%d", cap.value);
    CHECK(v == 0,         "hello: halt=%d", v);

    cvm_image_free(&img);
    free(blob);
}

static void test_syscall_two_args(void) {
    /* cvm_sys_add(13, 29) → 42 */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 13),
        enc_i16(CVM_OP_MOVI, 1, 29),
        enc_syscall(0),
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    const char *imports[] = { "cvm_sys_add" };
    size_t len;
    uint8_t *blob = build_blob_full(code, 4, 0, 0, imports, 1, NULL, 0, 0,
                                    NULL, 0, 0, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "two: load");
    CHECK(cvm_link(&img, "cvm_sys_add", sys_add, NULL) == CVM_OK, "two: link");
    int32_t v = 0;
    int r = cvm_run(&img, &v);
    CHECK(r == CVM_OK, "two: run %s", cvm_strerror(r));
    CHECK(v == 42,     "two: got %d", v);
    cvm_image_free(&img); free(blob);
}

static void test_syscall_print_string(void) {
    /* DATA contains "hi!\0" at offset 0; bytecode passes addr 0 to print_string. */
    static const uint8_t data[] = { 'h','i','!',0, 0,0,0,0 };
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 0),
        enc_syscall(0),
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    const char *imports[] = { "cvm_sys_print_string" };
    size_t len;
    uint8_t *blob = build_blob_full(code, 3, 0, 0, imports, 1,
                                    data, sizeof(data), 0,
                                    NULL, 0, 0, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "str: load");
    char captured[64] = {0};
    CHECK(cvm_link(&img, "cvm_sys_print_string", sys_print_string, captured) == CVM_OK,
          "str: link");
    int32_t v = 0;
    int r = cvm_run(&img, &v);
    CHECK(r == CVM_OK,                 "str: run %s", cvm_strerror(r));
    CHECK(strcmp(captured, "hi!") == 0,"str: captured='%s'", captured);
    CHECK(v == 3,                      "str: halt len=%d", v);
    cvm_image_free(&img); free(blob);
}

static void test_syscall_unlinked(void) {
    uint32_t code[] = {
        enc_syscall(0),
        enc_r(CVM_OP_HALT, 0, 0, 0),
    };
    const char *imports[] = { "cvm_sys_missing" };
    size_t len;
    uint8_t *blob = build_blob_full(code, 2, 0, 0, imports, 1, NULL, 0, 0,
                                    NULL, 0, 0, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "unlinked: load");
    int32_t v = 0;
    int r = cvm_run(&img, &v);
    CHECK(r == CVM_E_UNLINKED_SYSCALL, "unlinked: got %s", cvm_strerror(r));
    cvm_image_free(&img); free(blob);
}

static void test_syscall_no_such_import(void) {
    const char *imports[] = { "cvm_sys_real" };
    uint32_t code[] = { enc_r(CVM_OP_HALT, 0, 0, 0) };
    size_t len;
    uint8_t *blob = build_blob_full(code, 1, 0, 0, imports, 1, NULL, 0, 0,
                                    NULL, 0, 0, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "nosuch: load");
    int r = cvm_link(&img, "cvm_sys_imaginary", sys_print_int, NULL);
    CHECK(r == CVM_E_NO_SUCH_IMPORT, "nosuch: got %s", cvm_strerror(r));
    cvm_image_free(&img); free(blob);
}

static void test_syscall_trap(void) {
    uint32_t code[] = {
        enc_syscall(0),
        enc_r(CVM_OP_HALT, 0, 0, 0),
    };
    const char *imports[] = { "cvm_sys_trap" };
    size_t len;
    uint8_t *blob = build_blob_full(code, 2, 0, 0, imports, 1, NULL, 0, 0,
                                    NULL, 0, 0, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "trap: load");
    CHECK(cvm_link(&img, "cvm_sys_trap", sys_trap, NULL) == CVM_OK, "trap: link");
    int32_t v = 0;
    int r = cvm_run(&img, &v);
    CHECK(r == CVM_E_SYSCALL_TRAP, "trap: got %s", cvm_strerror(r));
    cvm_image_free(&img); free(blob);
}

static void test_builtin_heap_syscalls(void) {
    /* DATA = 8 bytes, BSS = 0, RESERVE = 1024.
     * cvm_sys_heap_start() should return 8 (start of reserve = end of static).
     * cvm_sys_heap_size()  should return 1024. */
    uint32_t code[] = {
        enc_syscall(0),                /* heap_start -> R0 */
        enc_r(CVM_OP_MOV, 1, 0, 0),    /* save in R1 */
        enc_syscall(1),                /* heap_size  -> R0 */
        enc_r(CVM_OP_ADD, 2, 0, 1),    /* sum = size + start = 1024 + 8 */
        enc_r(CVM_OP_HALT, 2, 0, 0),
    };
    const char *imports[] = { "cvm_sys_heap_start", "cvm_sys_heap_size" };
    static const uint8_t data8[] = { 0,0,0,0, 0,0,0,0 };

    size_t len;
    uint8_t *blob = build_blob_full(code, 5, 0, 0,
                                    imports, 2,
                                    data8, sizeof(data8),
                                    1024,
                                    NULL, 0, 0, &len);
    struct cvm_image img;
    int r = cvm_load(blob, len, &img);
    CHECK(r == CVM_OK, "heap_sys: load %s", cvm_strerror(r));
    CHECK(img.reserve_size == 1024, "heap_sys: reserve_size=%u", img.reserve_size);
    CHECK(img.heap_size == 8 + 1024, "heap_sys: heap_size=%u", img.heap_size);
    /* Both syscalls should be auto-bound. */
    CHECK(img.import_fns[0] != NULL, "heap_sys: heap_start unbound");
    CHECK(img.import_fns[1] != NULL, "heap_sys: heap_size  unbound");

    int32_t v = 0;
    r = cvm_run(&img, &v);
    CHECK(r == CVM_OK, "heap_sys: run %s", cvm_strerror(r));
    CHECK(v == 1024 + 8, "heap_sys: got %d", v);

    cvm_image_free(&img);
    free(blob);
}

static void test_movhi_wide_const(void) {
    /* MOVHI sets the upper 16 bits without disturbing the low 16. The
     * MOVI+MOVHI pair below should land 0x12345678 in R0. */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI,  0, (int16_t)0x5678),  /* low 16  -> R0 */
        enc_i16(CVM_OP_MOVHI, 0, (int16_t)0x1234),  /* high 16 -> R0 */
        enc_r  (CVM_OP_HALT,  0, 0, 0),
    };
    int32_t v = 0;
    int r = run_image(code, 3, 0, 0, &v);
    CHECK(r == CVM_OK, "movhi: %s", cvm_strerror(r));
    CHECK((uint32_t)v == 0x12345678u, "movhi: got 0x%x", (unsigned)v);

    /* And the canonical use case: load 65535 as an unsigned mask. */
    uint32_t code2[] = {
        enc_i16(CVM_OP_MOVI,  0, (int16_t)0xFFFF),  /* MOVI -1  -> R0 = 0xFFFFFFFF */
        enc_i16(CVM_OP_MOVHI, 0, 0),                /* zero high half -> R0 = 0xFFFF */
        enc_r  (CVM_OP_HALT,  0, 0, 0),
    };
    v = 0;
    r = run_image(code2, 3, 0, 0, &v);
    CHECK(r == CVM_OK, "movhi mask: %s", cvm_strerror(r));
    CHECK((uint32_t)v == 0xFFFFu, "movhi mask: got 0x%x", (unsigned)v);
}

static void test_null_func_ptr_call(void) {
    /* CALL imm24=0 must trap with CVM_E_NULL_FUNC_PTR even when there's
     * a FUNCS section with at least one user function. */
    uint32_t code[] = {
        enc_i24(CVM_OP_CALL, 0),         /* call NULL */
        enc_r  (CVM_OP_HALT, 0, 0, 0),   /* (unreachable) */
    };
    /* FUNCS table: slot 0 reserved (0), slot 1 = user func at PC=1 (the
     * HALT). The slot-0 trap fires before any indexing. */
    uint32_t funcs[] = { 0u, 1u };
    size_t len;
    uint8_t *blob = build_blob_full(code, 2, 0, 0,
                                    NULL, 0, NULL, 0, 0,
                                    funcs, 2, 64, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "null call: load");
    int32_t v = 0;
    int r = cvm_run(&img, &v);
    CHECK(r == CVM_E_NULL_FUNC_PTR, "null call: got %s", cvm_strerror(r));
    cvm_image_free(&img); free(blob);
}

static void test_null_func_ptr_callr(void) {
    /* CALLR with R[a]==0 traps the same way. */
    uint32_t code[] = {
        enc_i16(CVM_OP_MOVI, 0, 0),       /* R0 = 0 (NULL fn ptr) */
        enc_r  (CVM_OP_CALLR, 0, 0, 0),   /* indirect call through R0 */
        enc_r  (CVM_OP_HALT, 0, 0, 0),
    };
    uint32_t funcs[] = { 0u, 2u };
    size_t len;
    uint8_t *blob = build_blob_full(code, 3, 0, 0,
                                    NULL, 0, NULL, 0, 0,
                                    funcs, 2, 64, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "null callr: load");
    int32_t v = 0;
    int r = cvm_run(&img, &v);
    CHECK(r == CVM_E_NULL_FUNC_PTR, "null callr: got %s", cvm_strerror(r));
    cvm_image_free(&img); free(blob);
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
    test_cmp_signed();
    test_cmp_unsigned();
    test_div_signed();
    test_div_by_zero();
    test_shifts();
    test_bitwise();
    test_syscall_hello_int();
    test_syscall_two_args();
    test_syscall_print_string();
    test_syscall_unlinked();
    test_syscall_no_such_import();
    test_syscall_trap();
    test_builtin_heap_syscalls();
    test_movhi_wide_const();
    test_null_func_ptr_call();
    test_null_func_ptr_callr();

    if (g_failures) {
        fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
