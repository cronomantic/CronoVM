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
    CVM_E_BAD_IMPORTS,
    CVM_E_NO_SUCH_IMPORT,
    CVM_E_BAD_SYSCALL,
    CVM_E_UNLINKED_SYSCALL,
    CVM_E_SYSCALL_TRAP,
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
 *     SYSCALL  BC=imm16 (unsigned)         invoke import #imm16
 *     CMP_EQ   A=rd, B=rs1, C=rs2          R[A] = (R[B] == R[C]) ? 1 : 0
 *     CMP_NE                               R[A] = (R[B] != R[C]) ? 1 : 0
 *     CMP_LT                               R[A] = (R[B] <  R[C]) ? 1 : 0   (signed)
 *     CMP_LE                               R[A] = (R[B] <= R[C]) ? 1 : 0   (signed)
 *     CMP_LTU                              same as CMP_LT but unsigned
 *     CMP_LEU                              same as CMP_LE but unsigned
 *
 * All arithmetic is 32-bit two's-complement with wrap-around semantics.
 * Branch offsets are in instructions, relative to the instruction *after*
 * the branch (so offset 0 means fall through).
 *
 * SYSCALL invokes the host handler registered for import index imm16.
 * Args go in R0..R7, return value in R0. See docs/syscalls.md.
 * ------------------------------------------------------------------------- */

enum cvm_opcode {
    CVM_OP_HALT    = 0x00,
    CVM_OP_MOVI    = 0x01,
    CVM_OP_MOV     = 0x02,
    CVM_OP_ADD     = 0x03,
    CVM_OP_SUB     = 0x04,
    CVM_OP_MUL     = 0x05,
    CVM_OP_LDW     = 0x06,
    CVM_OP_STW     = 0x07,
    CVM_OP_JMP     = 0x08,
    CVM_OP_BEQ     = 0x09,
    CVM_OP_BNE     = 0x0A,
    CVM_OP_SYSCALL = 0x0B,

    /* Comparisons: R[A] = (R[B] cmp R[C]) ? 1 : 0
     * Signed by default; *_U variants treat operands as uint32_t. */
    CVM_OP_CMP_EQ  = 0x0C,
    CVM_OP_CMP_NE  = 0x0D,
    CVM_OP_CMP_LT  = 0x0E,
    CVM_OP_CMP_LE  = 0x0F,
    CVM_OP_CMP_LTU = 0x10,
    CVM_OP_CMP_LEU = 0x11,
};

#define CVM_REG_COUNT 256
#define CVM_SYSCALL_MAX_ARGS 8

struct cvm_image;

/* Host-provided syscall handler. Reads args from regs[0..N-1] (where N is the
 * arity declared by the host for that syscall) and writes the return value
 * to regs[0]. Returns 0 on success, nonzero to trap the VM with
 * CVM_E_SYSCALL_TRAP. See docs/syscalls.md. */
typedef int (*cvm_syscall_fn)(struct cvm_image *img,
                              int32_t *regs,
                              void *user_data);

struct cvm_image {
    uint32_t *code;
    uint32_t  code_count;

    /* heap[0 .. data_size) is initialised from the DATA section,
     * heap[data_size .. heap_size) is zero-filled from BSS. */
    uint8_t  *heap;
    uint32_t  heap_size;
    uint32_t  data_size;

    uint32_t  entry;

    /* Imports: parsed from the IMPORTS section. import_names[i] is the
     * NUL-terminated symbol the host should resolve via cvm_link. The fn /
     * userdata arrays are NULL until cvm_link sets them. */
    uint32_t          import_count;
    char            **import_names;
    cvm_syscall_fn   *import_fns;
    void            **import_userdata;
    char             *_import_blob;   /* internal: backing storage for names */
};

/* ---------------------------------------------------------------------------
 * Loader / lifecycle
 * ------------------------------------------------------------------------- */

int  cvm_load(const void *bytes, size_t len, struct cvm_image *out);
void cvm_image_free(struct cvm_image *img);
const char *cvm_strerror(int result);

/* ---------------------------------------------------------------------------
 * Linking — bind a host C function to the named import.
 *
 *   Returns CVM_OK on success, CVM_E_NO_SUCH_IMPORT if no import has that
 *   name. Repeated calls with the same name overwrite the prior handler.
 * ------------------------------------------------------------------------- */

int cvm_link(struct cvm_image *img,
             const char *name,
             cvm_syscall_fn fn,
             void *user_data);

/* ---------------------------------------------------------------------------
 * Heap helpers (bounds-checked) for syscalls that read/write VM memory.
 * Return CVM_OK on success or CVM_E_BAD_ADDR on out-of-range access.
 * ------------------------------------------------------------------------- */

int cvm_heap_read (struct cvm_image *img, uint32_t addr, void *out, size_t n);
int cvm_heap_write(struct cvm_image *img, uint32_t addr, const void *in, size_t n);

/* ---------------------------------------------------------------------------
 * Run img to completion. On CVM_OK, *return_value (if non-null) holds the
 * value of the register named in the HALT instruction's A field.
 *
 * cvm_run_args additionally seeds R[0..arg_count-1] with the supplied
 * values before the first instruction executes — useful while a proper
 * CALL/RET ABI is still being designed and the host needs to invoke a
 * translated function with arguments.
 * ------------------------------------------------------------------------- */

int  cvm_run(struct cvm_image *img, int32_t *return_value);
int  cvm_run_args(struct cvm_image *img,
                  const int32_t *args, uint32_t arg_count,
                  int32_t *return_value);

#ifdef __cplusplus
}
#endif

#endif /* CVM_H */
