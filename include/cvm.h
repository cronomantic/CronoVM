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

/* CronoVM library release version. Distinct from CVM_VERSION_1_0
 * above — that's the binary-format magic number a `.bin` carries.
 * CVM_VERSION_* names the build of the library / toolchain itself, so
 * a downstream consumer can log it or compile-time check against a
 * minimum. CMake's project(cronovm VERSION ...) bakes the real values
 * in via target_compile_definitions; the fallbacks here only kick in
 * for ad-hoc compilations without CMake. */
#ifndef CVM_VERSION_MAJOR
#  define CVM_VERSION_MAJOR 0
#endif
#ifndef CVM_VERSION_MINOR
#  define CVM_VERSION_MINOR 1
#endif
#ifndef CVM_VERSION_PATCH
#  define CVM_VERSION_PATCH 0
#endif
#ifndef CVM_VERSION_STRING
#  define CVM_VERSION_STRING "0.1.0"
#endif

const char *cvm_version_string(void);          /* "0.1.0" etc. */
uint32_t    cvm_version_number(void);          /* (M<<16) | (m<<8) | p */

enum cvm_section_type {
    CVM_SEC_CODE          = 1,
    CVM_SEC_DATA          = 2,
    CVM_SEC_BSS           = 3,
    CVM_SEC_IMPORTS       = 4,
    CVM_SEC_DEBUG         = 5,
    CVM_SEC_HEAP_RESERVE  = 6,   /* file_off=0, size = free-region bytes */
    CVM_SEC_STACK_RESERVE = 7,   /* file_off=0, size = stack region bytes */
    CVM_SEC_FUNCS         = 8,   /* u32[N] — entry instruction index per func */
    CVM_SEC_HOST_REGION   = 9,   /* named host-shared regions; payload is
                                  * u32 region_count followed by 28-byte
                                  * entries: name[16] + size + direction +
                                  * flags. Loader carves space inside
                                  * mem_size and exposes offsets via
                                  * cvm_image_get_region / cvm_sys_get_region. */
    CVM_SEC_ROM           = 10,  /* read-only cartridge data baked into the
                                  * .bin (e.g. a game WAD). Payload bytes are
                                  * copied into the heap after DATA/BSS/REGIONS;
                                  * the program reads them as a pointer and
                                  * discovers base/size via cvm_sys_rom_base /
                                  * cvm_sys_rom_size. Read-only by convention
                                  * (the VM enforces only heap bounds). */
};

enum cvm_region_dir {
    CVM_REGION_R  = 1,   /* host writes, VM reads (input, textures) */
    CVM_REGION_W  = 2,   /* VM writes, host reads (framebuffer, audio) */
    CVM_REGION_RW = 3,   /* shared in both directions (command buffer) */
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
    CVM_E_DIV_BY_ZERO,
    CVM_E_BAD_FUNCS,        /* malformed FUNCS section */
    CVM_E_BAD_FUNC_INDEX,   /* CALL imm24 outside func table */
    CVM_E_STACK_OVERFLOW,   /* SP went past the stack region */
    CVM_E_NULL_FUNC_PTR,    /* CALL/CALLR targeted the reserved slot 0 */
    CVM_E_BAD_REGION,       /* malformed CVM_SEC_HOST_REGION section */
    CVM_E_NO_SUCH_REGION,   /* cvm_image_get_region on an unknown name */
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
 *     MOVHI A=rd, BC=imm16                 R[A] = (imm16 << 16) | (R[A] & 0xFFFF)
 *     MEMCPY  A=rdst, B=rsrc, C=rlen       memcpy (heap+R[A], heap+R[B], R[C])
 *     MEMSET  A=rdst, B=rval, C=rlen       memset (heap+R[A], R[B] & 0xFF, R[C])
 *     MEMMOVE A=rdst, B=rsrc, C=rlen       memmove(heap+R[A], heap+R[B], R[C])
 *     MULH   A=rd, B=rs1, C=rs2            R[A] = upper32(int64_t)(R[B] * R[C])   (signed)
 *     MULHU  A=rd, B=rs1, C=rs2            R[A] = upper32(uint64_t)(R[B] * R[C])  (unsigned)
 *
 *     FADD/FSUB/FMUL/FDIV  A=rd, B=rs1, C=rs2  IEEE 754 binary32 arith
 *     FNEG  A=rd, B=rs1                        flip sign bit
 *     FCMP_EQ/NE/LT/LE  A=rd, B=rs1, C=rs2     EQ/LT/LE are ordered (NaN→0);
 *                                              NE is unordered (NaN→1) — matches C
 *     F2I_S / F2I_U  A=rd, B=rs1               saturating: NaN→0, ±overflow→INT_*
 *     I2F_S / I2F_U  A=rd, B=rs1               int → float, round-to-nearest-even
 *     JMPR  A=rs1                              pc = (uint32_t)R[A], bounds-checked
 *     MOV   A=rd, B=rs1                    R[A] = R[B]
 *     ADD   A=rd, B=rs1, C=rs2             R[A] = R[B] + R[C]
 *     SUB                                  R[A] = R[B] - R[C]
 *     MUL                                  R[A] = R[B] * R[C]
 *     LDW   A=rd, B=rs1                    R[A] = *(i32*)(heap + R[B])
 *     STW   B=rs1, C=rs2                   *(i32*)(heap + R[B]) = R[C]
 *     LDB   A=rd, B=rs1                    R[A] = (u32)*(u8*)(heap + R[B])
 *     STB   B=rs1, C=rs2                   *(u8*)(heap + R[B]) = R[C] & 0xFF
 *     LDH   A=rd, B=rs1                    R[A] = (u32)*(u16*)(heap + R[B])
 *     STH   B=rs1, C=rs2                   *(u16*)(heap + R[B]) = R[C] & 0xFFFF
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
 *     DIV/DIVU A=rd, B=rs1, C=rs2          signed/unsigned division; trap on R[C]==0
 *     MOD/MODU A=rd, B=rs1, C=rs2          signed/unsigned remainder; trap on R[C]==0
 *     SHL      A=rd, B=rs1, C=rs2          R[A] = R[B] << (R[C] & 31)
 *     SHR/SAR                              logical / arithmetic right shift
 *     AND/OR/XOR                           bitwise
 *     CALL     ABC=imm24 (unsigned)        push pc+1; pc = FUNCS[imm24]
 *     CALLR    A=rs1                       push pc+1; pc = FUNCS[R[A]]
 *     RET                                  pc = pop(); halt if pc == 0xFFFFFFFF
 *
 *   FUNCS[0] is reserved as the null-function-pointer slot. CALL imm24=0
 *   and CALLR Rd with R[d]==0 trap with CVM_E_NULL_FUNC_PTR; user functions
 *   live at FUNCS[1..N].
 *
 * All arithmetic is 32-bit two's-complement with wrap-around semantics.
 * Branch offsets are in instructions, relative to the instruction *after*
 * the branch (so offset 0 means fall through).
 *
 * SYSCALL invokes the host handler registered for import index imm16.
 * Args go in R0..R7, return value in R0. See docs/syscalls.md.
 *
 * CALL/RET use a stack region addressed via R255 (SP). On run start, R255 is
 * set to the top of memory and the sentinel 0xFFFFFFFF is pushed as the
 * outermost return PC; when RET pops it, the run ends and R[0] is returned.
 * The user-call ABI mirrors the syscall ABI for the first 8 args (R0..R7);
 * any remaining args are pushed on the stack by the caller (lowest stacked
 * arg at lowest address) and cleaned up by the caller after RET. Return
 * value lands in R0.
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

    /* Arithmetic completion. DIV/MOD trap on R[C]==0; INT_MIN/-1 wraps to
     * INT_MIN, INT_MIN%-1 yields 0 (so wrap is total). Shifts mask the
     * amount to its low 5 bits, matching how most retro hardware behaves. */
    CVM_OP_DIV     = 0x12,    /* signed */
    CVM_OP_DIVU    = 0x13,    /* unsigned */
    CVM_OP_MOD     = 0x14,    /* signed remainder */
    CVM_OP_MODU    = 0x15,    /* unsigned remainder */
    CVM_OP_SHL     = 0x16,
    CVM_OP_SHR     = 0x17,    /* logical shift right */
    CVM_OP_SAR     = 0x18,    /* arithmetic shift right */
    CVM_OP_AND     = 0x19,
    CVM_OP_OR      = 0x1A,
    CVM_OP_XOR     = 0x1B,

    /* Calls. CALL pushes the address of the next instruction to the stack
     * (decrementing R255 by 4) and jumps to FUNCS[imm24]. RET pops a 32-bit
     * value from the stack and uses it as the new pc; if the popped value is
     * the sentinel 0xFFFFFFFF, execution halts and the run returns R[0].
     * CALLR is identical to CALL but reads the function index from R[A]
     * — used for function pointers / indirect calls. */
    CVM_OP_CALL    = 0x1C,    /* ABC = imm24 (unsigned function index) */
    CVM_OP_RET     = 0x1D,
    CVM_OP_CALLR   = 0x1E,    /* A = rs1 (holds function index) */

    /* Sub-word memory ops. Loads zero-extend; sign extension is the
     * translator's job (an explicit SHL/SAR pair on the loaded value).
     * Stores write only the low byte / halfword and ignore upper bits. */
    CVM_OP_LDB     = 0x1F,
    CVM_OP_STB     = 0x20,
    CVM_OP_LDH     = 0x21,
    CVM_OP_STH     = 0x22,

    /* Wide-constant materialisation. MOVHI sets the upper 16 bits of R[A]
     * from imm16 without touching the lower 16, so the translator can load
     * any 32-bit immediate as `MOVI rd, lo16; MOVHI rd, hi16`. */
    CVM_OP_MOVHI   = 0x23,

    /* Block memory ops. All three forms take three register operands:
     *   A = destination address, B = source address (or fill value for
     *   MEMSET), C = length in bytes. Length is read as uint32. Bounds
     *   are checked once for each region against [0, mem_size); a length
     *   of zero succeeds without touching memory.
     *
     *   MEMCPY   — copy R[C] bytes from heap+R[B] to heap+R[A].
     *              Behaviour on overlap is undefined; use MEMMOVE if the
     *              regions may overlap.
     *   MEMSET   — fill R[C] bytes at heap+R[A] with the byte R[B] & 0xFF.
     *   MEMMOVE  — like MEMCPY but overlap-safe in both directions.
     *
     * These delegate to the host's memcpy/memset/memmove (typically SIMD)
     * after a single bounds check, so a block move costs one dispatch and
     * a libc call instead of one dispatch per byte/word. */
    CVM_OP_MEMCPY  = 0x24,
    CVM_OP_MEMSET  = 0x25,
    CVM_OP_MEMMOVE = 0x26,

    /* High-half multiply. MUL gives the low 32 bits of a 32×32 product;
     * MULH / MULHU give the upper 32 bits. With (MUL, MULH) you compose
     * Q16.16 fixed-point multiply at full precision in four instructions
     * (MUL, MULH, two shifts) without ever touching a 64-bit value — the
     * canonical embedded-ISA primitive (ARM SMULL/UMULL, MIPS mult/mfhi,
     * RISC-V MULH).
     *   MULH   A=rd, B=rs1, C=rs2   R[A] = (i32)(((i64)R[B] * (i64)R[C]) >> 32)   (signed)
     *   MULHU  A=rd, B=rs1, C=rs2   R[A] = (i32)(((u64)R[B] * (u64)R[C]) >> 32)   (unsigned)
     * The translator surfaces them as `cvm_intrin_mulh` / `cvm_intrin_mulhu`
     * extern decls (see runtime/lib/cvm_intrin.h); C code that uses
     * cvm_mulh(a, b) compiles to a single MULH instead of a CALL. */
    CVM_OP_MULH    = 0x27,
    CVM_OP_MULHU   = 0x28,

    /* IEEE 754 single-precision float ops. f32 values share the i32
     * register file: a register holds 32 bits and is reinterpreted as
     * float for the duration of the opcode (host-side memcpy/union).
     * No `f64` opcodes — `double` is rejected by the translator's type
     * subset, and a runtime header (cvm_float64.h) provides software
     * emulation for code that genuinely needs it.
     *
     *   FADD/FSUB/FMUL/FDIV  A=rd, B=rs1, C=rs2   IEEE 754 binary32 arithmetic.
     *                                              FDIV by zero produces ±Inf
     *                                              (no trap); NaN propagates.
     *   FNEG                 A=rd, B=rs1          flip the sign bit.
     *   FCMP_EQ              A=rd, B=rs1, C=rs2   ordered ==  (NaN → 0)
     *   FCMP_NE              A=rd, B=rs1, C=rs2   unordered != (NaN → 1; matches
     *                                              C `a != b`).
     *   FCMP_LT/FCMP_LE      A=rd, B=rs1, C=rs2   ordered <, ≤ (NaN → 0).
     *   F2I_S / F2I_U        A=rd, B=rs1          float → int32 / uint32, with
     *                                              saturating semantics: NaN → 0,
     *                                              +Inf or > max → INT_MAX /
     *                                              UINT_MAX, -Inf or < min →
     *                                              INT_MIN / 0. Portable across
     *                                              hosts (raw `(int)f` would not
     *                                              be — x86 indefinite vs ARM
     *                                              saturating).
     *   I2F_S / I2F_U        A=rd, B=rs1          int32 / uint32 → float; result
     *                                              rounded to nearest-even when
     *                                              the magnitude exceeds 2^24.
     *   FSQRT                A=rd, B=rs1          single-precision square root via
     *                                              the host's `sqrtf()`. Negative
     *                                              inputs and NaN propagate to
     *                                              NaN; sqrt(±0)=±0; sqrt(+Inf)=+Inf.
     *                                              Surfaced by the translator from
     *                                              `cvm_intrin_fsqrt` (see
     *                                              runtime/lib/cvm_intrin.h). */
    CVM_OP_FADD    = 0x29,
    CVM_OP_FSUB    = 0x2A,
    CVM_OP_FMUL    = 0x2B,
    CVM_OP_FDIV    = 0x2C,
    CVM_OP_FNEG    = 0x2D,
    CVM_OP_FCMP_EQ = 0x2E,
    CVM_OP_FCMP_NE = 0x2F,
    CVM_OP_FCMP_LT = 0x30,
    CVM_OP_FCMP_LE = 0x31,
    CVM_OP_F2I_S   = 0x32,
    CVM_OP_F2I_U   = 0x33,
    CVM_OP_I2F_S   = 0x34,
    CVM_OP_I2F_U   = 0x35,

    /* Indirect computed jump. Sets pc = (uint32_t)R[A], bounds-checked
     * against code_count. Distinct from CALLR (which indexes FUNCS and
     * pushes a return PC); JMPR is the dense-switch / jump-table
     * dispatcher's primitive. The translator only emits it as the tail
     * of a jump-table sequence; the table entries are absolute
     * instruction indices patched in after branch relaxation. */
    CVM_OP_JMPR    = 0x36,

    /* Single-precision square root. Sole f32 unary that's not just a sign
     * tweak — uses the host's `sqrtf()` rather than fold to a single ALU
     * op like FNEG does. Surfaced by the translator from `cvm_intrin_fsqrt`
     * in cvm_intrin.h; users call `cvm_fsqrt(x)`. */
    CVM_OP_FSQRT   = 0x37,

    /* Q16.16 fixed-point divide: R[A] = (u32)((((u64)(u32)R[B]) << 16) /
     * (u32)R[C]); traps on R[C]==0 like DIV/DIVU. The 64-bit numerator and
     * the divide happen in one host op, so a fixed-point quotient costs a
     * single instruction instead of a software 48-bit long division. The
     * divide sibling of MULH's Q16.16 multiply; surfaced by the translator
     * from `cvm_intrin_qdiv_16_16` (cvm_intrin.h), users call
     * `cvm_qdiv_16_16(a, b)`. Operands are unsigned magnitudes — callers
     * (e.g. DOOM's FixedDiv) apply sign and the overflow guard themselves. */
    CVM_OP_QDIV1616 = 0x38,
};

#define CVM_REG_COUNT 256
#define CVM_SYSCALL_MAX_ARGS 8
#define CVM_REG_SP    255u
#define CVM_RET_SENTINEL 0xFFFFFFFFu

struct cvm_image;

/* Host-provided syscall handler. Reads args from regs[0..N-1] (where N is the
 * arity declared by the host for that syscall) and writes the return value
 * to regs[0]. Returns 0 on success, nonzero to trap the VM with
 * CVM_E_SYSCALL_TRAP. See docs/syscalls.md. */
typedef int (*cvm_syscall_fn)(struct cvm_image *img,
                              int32_t *regs,
                              void *user_data);

struct cvm_region {
    char     name[16];        /* NUL-terminated within 16 bytes */
    uint32_t offset;          /* assigned by the loader, into img->heap */
    uint32_t size;
    uint32_t direction;       /* one of cvm_region_dir; informational only */
};

struct cvm_image {
    uint32_t *code;
    uint32_t  code_count;

    /* Memory layout (single contiguous buffer of mem_size bytes):
     *   heap[0 .. data_size)                            DATA (initialised)
     *   heap[data_size .. data_size + bss_size)         BSS  (zero-filled)
     *   heap[... .. ... + region_total)                 REGIONS (zero-filled;
     *                                                    named host-shared
     *                                                    slices, offsets via
     *                                                    cvm_image_get_region)
     *   heap[heap_size - reserve_size .. heap_size)     RESERVE (zero-filled,
     *                                                    free for the user
     *                                                    allocator)
     *   heap[heap_size .. heap_size + stack_size)       STACK (zero-filled,
     *                                                    grows down from top)
     *
     * heap_size names just the heap portion (data+bss+regions+reserve). LDW/STW
     * bounds-check against mem_size = heap_size + stack_size so stack
     * accesses use the same opcodes as heap accesses. cvm_sys_heap_start
     * still returns heap_size - reserve_size; the stack region is invisible
     * to the user-side allocator.
     */
    uint8_t  *heap;
    uint32_t  heap_size;
    uint32_t  data_size;
    uint32_t  reserve_size;
    uint32_t  stack_size;
    uint32_t  mem_size;       /* heap_size + stack_size, for bounds checks */

    /* Host-shared regions, parsed from CVM_SEC_HOST_REGION. Empty if the
     * binary doesn't declare any. Use cvm_image_get_region for lookups. */
    struct cvm_region *regions;
    uint32_t           region_count;

    /* Read-only cartridge ROM, parsed from CVM_SEC_ROM. rom_size is 0 when
     * the binary carries no ROM. When present, the bytes live at
     * heap + rom_offset; the host can read or pre-fill them there, and the
     * program discovers the same offset via cvm_sys_rom_base. */
    uint32_t  rom_offset;
    uint32_t  rom_size;

    /* Allocator captured at cvm_load_ex time; used by cvm_image_free
     * to release the image's owned memory. Both function pointers
     * being NULL means "use stdlib malloc/free" — that's the
     * effective shape produced by cvm_load (no allocator). */
    struct {
        void *(*alloc_fn)(size_t bytes, void *user_data);
        void  (*free_fn) (void *ptr,    void *user_data);
        void   *user_data;
    } allocator;

    uint32_t  entry;

    /* FUNCS section: img->func_offsets[i] is the instruction index in CODE
     * where function #i starts, used as the target of CALL imm24. NULL if
     * the binary has no FUNCS section (i.e. emits no CALL instructions). */
    uint32_t *func_offsets;
    uint32_t  func_count;

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
 * Pluggable allocator. Embedded targets often have no `malloc`, custom
 * pool allocators (FreeRTOS heap_4, Zephyr k_malloc, picolibc), or
 * fixed memory budgets, so the load path takes a `cvm_allocator_t`
 * with two function pointers. Either pointer may be NULL to fall
 * through to the stdlib equivalent; passing the whole allocator as
 * NULL is identical to calling cvm_load.
 *
 * The image stashes the allocator at load time so `cvm_image_free`
 * uses the matching free for every block it returns. The allocator's
 * `user_data` is opaque — pass whatever the alloc/free implementations
 * need (a pool pointer, a stats counter, …).
 *
 * After a successful cvm_load_ex the binary holds these allocations:
 *   - one block for the code array (4 × code_count bytes),
 *   - one block for the heap (mem_size bytes; may be zero),
 *   - one block for the imports name blob (when IMPORTS is present),
 *   - up to three small blocks for import name pointers / handlers /
 *     userdata,
 *   - one block for the FUNCS table (when present),
 *   - one block for the host_region descriptors (when present).
 * Zero-size sections allocate nothing. The host can therefore
 * pre-budget the worst-case allocation count if it's running with a
 * fixed-pool allocator. */

typedef struct {
    void *(*alloc_fn)(size_t bytes, void *user_data);
    void  (*free_fn) (void *ptr,    void *user_data);
    void   *user_data;
} cvm_allocator_t;

int cvm_load_ex(const void *bytes, size_t len,
                struct cvm_image *out,
                const cvm_allocator_t *allocator);

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
 * Host-shared region lookup. `name` matches a CVM_SEC_HOST_REGION entry
 * declared by the binary. On success returns CVM_OK and writes the region's
 * heap-relative offset and size to the out pointers (either may be NULL).
 * Returns CVM_E_NO_SUCH_REGION if no region of that name exists. The host
 * accesses the region's bytes at `img->heap + *out_offset`.
 * ------------------------------------------------------------------------- */

int cvm_image_get_region(struct cvm_image *img,
                         const char *name,
                         uint32_t *out_offset,
                         uint32_t *out_size);

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

/* ---------------------------------------------------------------------------
 * Call a function registered in the FUNCS table without re-entering through
 * the image's entry point. fn_index is 1..func_count-1 (FUNCS[0] is the
 * reserved null slot); args are seeded into R0..R7 as for cvm_run_args; any
 * args beyond 8 must already be on the stack at the time of the call. The
 * VM runs until the called function's RET pops the run-completion sentinel,
 * and *return_value receives R[0] at that point.
 *
 * Each call gets a fresh register file (the only state that persists across
 * calls is img->heap), so a host driving a per-frame callback can invoke
 * cvm_call(img, frame_fn, NULL, 0, NULL) every 1/60 s without worrying
 * about leaked register state.
 *
 * Errors: CVM_E_BAD_FUNCS if the image has no FUNCS section,
 * CVM_E_BAD_FUNC_INDEX if fn_index is out of range, CVM_E_NULL_FUNC_PTR if
 * fn_index is 0.
 * ------------------------------------------------------------------------- */
int  cvm_call(struct cvm_image *img,
              uint32_t fn_index,
              const int32_t *args, uint32_t arg_count,
              int32_t *return_value);

#ifdef __cplusplus
}
#endif

#endif /* CVM_H */
