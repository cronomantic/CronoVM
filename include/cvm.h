#ifndef CVM_H
#define CVM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Binary format (little-endian on disk, version 1.0)
 *
 *   File header (24 bytes)
 *     0   magic[4]            'C','V','M','1'
 *     4   version             u32     0x00010000 = 1.0
 *     8   flags               u32     reserved, must be 0
 *    12   section_count       u32
 *    16   section_table_off   u32     absolute offset of the section table
 *    20   entry               u32     instruction index into the CODE section
 *
 *   Section entry (16 bytes; section_count of these at section_table_off)
 *     0   type                u32     see cvm_section_type
 *     4   file_off            u32     absolute offset (0 for BSS)
 *     8   size                u32     bytes (for BSS: virtual size)
 *    12   flags               u32     reserved, must be 0
 *
 *   Each section type appears at most once except DEBUG. CODE is required.
 *   CODE size must be a multiple of 4.
 * ------------------------------------------------------------------------- */

#define CVM_VERSION_1_0   0x00010000u

enum cvm_section_type {
    CVM_SEC_CODE    = 1,
    CVM_SEC_DATA    = 2,
    CVM_SEC_BSS     = 3,
    CVM_SEC_IMPORTS = 4,
    CVM_SEC_DEBUG   = 5,
};

enum cvm_result {
    CVM_OK = 0,
    CVM_E_TRUNCATED,
    CVM_E_BAD_MAGIC,
    CVM_E_BAD_VERSION,
    CVM_E_BAD_SECTION,
    CVM_E_DUP_SECTION,
    CVM_E_NO_CODE,
    CVM_E_BAD_ENTRY,
    CVM_E_NOMEM,
};

struct cvm_image {
    uint32_t *code;
    uint32_t  code_count;

    /* heap[0 .. data_size) is initialised from the DATA section,
     * heap[data_size .. heap_size) is zero-filled from BSS. */
    uint8_t  *heap;
    uint32_t  heap_size;
    uint32_t  data_size;

    uint32_t  entry;

    /* Raw IMPORTS blob — shape is deferred until the syscall ABI is pinned. */
    void     *imports;
    uint32_t  imports_size;
};

int  cvm_load(const void *bytes, size_t len, struct cvm_image *out);
void cvm_image_free(struct cvm_image *img);
const char *cvm_strerror(int result);

#ifdef __cplusplus
}
#endif

#endif /* CVM_H */
