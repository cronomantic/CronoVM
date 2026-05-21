/* test_rom — exercises CVM_SEC_ROM (read-only cartridge data).
 *
 * A cart bakes a data blob into its .bin; the loader copies it into the
 * heap and exposes its offset/size via cvm_sys_rom_base / cvm_sys_rom_size.
 * The program reads it as ordinary memory. This is how a DOOM-class cart
 * ships its WAD. Contract checked here:
 *   1. img.rom_offset / img.rom_size are populated from the section.
 *   2. The ROM bytes land in the heap at rom_offset, byte-for-byte.
 *   3. The built-in syscalls return the matching base/size, and a program
 *      can LDB through the base pointer to read a ROM byte.
 *
 * Hand-assembled blob — independent of the translator. */

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

static uint32_t enc_r(uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    return (uint32_t)op | ((uint32_t)a<<8) | ((uint32_t)b<<16) | ((uint32_t)c<<24);
}
static uint32_t enc_syscall(uint16_t id) {
    return (uint32_t)CVM_OP_SYSCALL | ((uint32_t)id << 16);
}
static void put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

/* Build a blob: CODE + IMPORTS(2) + ROM(payload). */
static uint8_t *build(const uint32_t *code, uint32_t code_count, uint32_t entry,
                      const char *const *imports, uint32_t import_count,
                      const uint8_t *rom, uint32_t rom_size, size_t *out_len)
{
    uint32_t imports_size = 4u + import_count*4u;
    for (uint32_t i = 0; i < import_count; ++i)
        imports_size += (uint32_t)strlen(imports[i]) + 1u;

    uint32_t sec_count = 1u + (import_count>0?1u:0u) + (rom_size>0?1u:0u);
    uint32_t table_off = 24;
    uint32_t cursor    = table_off + sec_count*16u;
    uint32_t code_off  = cursor;            uint32_t code_size = code_count*4u; cursor += code_size;
    uint32_t imports_off = import_count>0 ? cursor : 0; cursor += (import_count>0?imports_size:0);
    uint32_t rom_off   = rom_size>0 ? cursor : 0;       cursor += rom_size;
    size_t total = cursor;

    uint8_t *buf = (uint8_t*)calloc(1, total);
    buf[0]='C';buf[1]='V';buf[2]='M';buf[3]='1';
    put_u32(buf+4, 0x00010000u); put_u32(buf+8,0); put_u32(buf+12, sec_count);
    put_u32(buf+16, table_off);  put_u32(buf+20, entry);

    uint32_t te = table_off;
    put_u32(buf+te+0, CVM_SEC_CODE); put_u32(buf+te+4, code_off); put_u32(buf+te+8, code_size); te+=16;
    if (import_count>0) { put_u32(buf+te+0, CVM_SEC_IMPORTS); put_u32(buf+te+4, imports_off); put_u32(buf+te+8, imports_size); te+=16; }
    if (rom_size>0)     { put_u32(buf+te+0, CVM_SEC_ROM);     put_u32(buf+te+4, rom_off);     put_u32(buf+te+8, rom_size);     te+=16; }

    for (uint32_t i=0;i<code_count;++i) put_u32(buf+code_off+i*4u, code[i]);
    if (import_count>0) {
        uint8_t *p = buf+imports_off; put_u32(p, import_count);
        uint32_t nc = 4u + import_count*4u;
        for (uint32_t i=0;i<import_count;++i){ put_u32(p+4u+i*4u, nc); size_t L=strlen(imports[i]); memcpy(p+nc, imports[i], L+1); nc+=(uint32_t)L+1u; }
    }
    if (rom_size>0) memcpy(buf+rom_off, rom, rom_size);
    *out_len = total;
    return buf;
}

static void test_rom_loaded(void) {
    static const uint8_t rom[] = { 'W','A','D',0, 0xDE,0xAD,0xBE,0xEF, 1,2,3,4 };
    /* Program: r0 = rom_base; r0 = LDB[r0] (first ROM byte 'W'=0x57); HALT r0. */
    uint32_t code[] = {
        enc_syscall(0),               /* cvm_sys_rom_base -> R0 */
        enc_r(CVM_OP_LDB, 0, 0, 0),   /* R0 = (u8)heap[R0] */
        enc_r(CVM_OP_HALT, 0, 0, 0),
    };
    const char *imports[] = { "cvm_sys_rom_base", "cvm_sys_rom_size" };
    size_t len;
    uint8_t *blob = build(code, 3, 0, imports, 2, rom, sizeof(rom), &len);

    struct cvm_image img;
    int rc = cvm_load(blob, len, &img);
    CHECK(rc == CVM_OK, "load: %s", cvm_strerror(rc));
    CHECK(img.rom_size == sizeof(rom), "rom_size=%u want %zu", img.rom_size, sizeof(rom));
    /* rom_offset may legitimately be 0 when the binary has no DATA/BSS/regions
     * before it — the cart distinguishes "no ROM" by rom_size==0, not base.
     * ROM bytes are copied verbatim into the heap at rom_offset. */
    CHECK(memcmp(img.heap + img.rom_offset, rom, sizeof(rom)) == 0, "rom bytes mismatch");

    int32_t v = 0;
    rc = cvm_run(&img, &v);
    CHECK(rc == CVM_OK, "run: %s", cvm_strerror(rc));
    CHECK(v == 'W', "rom[0] via LDB = %d want %d", v, 'W');

    cvm_image_free(&img); free(blob);
}

static void test_no_rom(void) {
    /* A binary without a ROM section: rom_size 0, builtins return 0. */
    uint32_t code[] = {
        enc_syscall(0),               /* rom_size -> R0 */
        enc_r(CVM_OP_HALT, 0, 0, 0),
    };
    const char *imports[] = { "cvm_sys_rom_size" };
    size_t len;
    uint8_t *blob = build(code, 2, 0, imports, 1, NULL, 0, &len);
    struct cvm_image img;
    CHECK(cvm_load(blob, len, &img) == CVM_OK, "no-rom: load");
    CHECK(img.rom_size == 0, "no-rom: rom_size=%u", img.rom_size);
    int32_t v = -1;
    CHECK(cvm_run(&img, &v) == CVM_OK, "no-rom: run");
    CHECK(v == 0, "no-rom: rom_size syscall = %d want 0", v);
    cvm_image_free(&img); free(blob);
}

int main(void) {
    test_rom_loaded();
    test_no_rom();
    if (g_failures) { fprintf(stderr, "%d failures\n", g_failures); return 1; }
    fprintf(stderr, "test_rom: ok\n");
    return 0;
}
