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
    CVM_E_BAD_OPCODE,
    CVM_E_BAD_PC,
    CVM_E_BAD_ADDR,
};

/* ---------------------------------------------------------------------------
 * Instruction set (v1.0)
 *
 * Every instruction is 32 bits, little-endian:
 *     [ opcode:8 | A:8 | B:8 | C:8 ]
 *
 * Field interpretation per opcode (R = 256-entry i32 register file):
 *     HALT  A=rs1                          stop, return R[A]
 *     MOVI  A=rd, BC=imm16 (signed)        R[A] = sext(imm16)
 *     MOV   A=rd, B=rs1                    R[A] = R[B]
 *     ADD   A=rd, B=rs1, C=rs2             R[A] = R[B] + R[C]
 *     SUB                                  R[A] = R[B] - R[C]
 *     MUL                                  R[A] = R[B] * R[C]
 *     LDW   A=rd, B=rs1                    R[A] = *(i32*)(heap + R[B])
 *     STW   B=rs1, C=rs2                   *(i32*)(heap + R[B]) = R[C]
 *     JMP   ABC=imm24 (signed)             pc += imm24   (relative to next ins)
 *     BEQ   A=rs1, B=rs2, C=imm8 (signed)  if R[A]==R[B] pc += imm8
 *     BNE                                  if R[A]!=R[B] pc += imm8
 *
 * All arithmetic is 32-bit two's-complement with wrap-around semantics.
 * Branch offsets are in instructions, relative to the instruction *after*
 * the branch (so offset 0 means fall through).
 * ------------------------------------------------------------------------- */

enum cvm_opcode {
    CVM_OP_HALT = 0x00,
    CVM_OP_MOVI = 0x01,
    CVM_OP_MOV  = 0x02,
    CVM_OP_ADD  = 0x03,
    CVM_OP_SUB  = 0x04,
    CVM_OP_MUL  = 0x05,
    CVM_OP_LDW  = 0x06,
    CVM_OP_STW  = 0x07,
    CVM_OP_JMP  = 0x08,
    CVM_OP_BEQ  = 0x09,
    CVM_OP_BNE  = 0x0A,
};

#define CVM_REG_COUNT 256

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

/* Run img to completion. On CVM_OK, *return_value (if non-null) holds the
 * value of the register named in the HALT instruction's A field. */
int  cvm_run(struct cvm_image *img, int32_t *return_value);

#ifdef __cplusplus
}
#endif

#endif /* CVM_H */
