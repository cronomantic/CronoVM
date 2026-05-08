#include "cvm.h"

#include <stdlib.h>
#include <string.h>

#define CVM_HEADER_SIZE   24u
#define CVM_SECTION_SIZE  16u
#define CVM_MAX_SEC_TYPE  5u

static uint32_t read_u32_le(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int cvm_load(const void *bytes, size_t len, struct cvm_image *out) {
    if (!bytes || !out) return CVM_E_TRUNCATED;
    memset(out, 0, sizeof(*out));

    if (len < CVM_HEADER_SIZE) return CVM_E_TRUNCATED;
    const uint8_t *base = (const uint8_t *)bytes;

    if (base[0] != 'C' || base[1] != 'V' || base[2] != 'M' || base[3] != '1')
        return CVM_E_BAD_MAGIC;

    uint32_t version           = read_u32_le(base + 4);
    uint32_t flags             = read_u32_le(base + 8);
    uint32_t section_count     = read_u32_le(base + 12);
    uint32_t section_table_off = read_u32_le(base + 16);
    uint32_t entry             = read_u32_le(base + 20);

    if (version != CVM_VERSION_1_0) return CVM_E_BAD_VERSION;
    if (flags != 0)                 return CVM_E_BAD_SECTION;

    uint64_t table_end = (uint64_t)section_table_off
                       + (uint64_t)section_count * CVM_SECTION_SIZE;
    if (table_end > len) return CVM_E_TRUNCATED;

    uint8_t  seen[CVM_MAX_SEC_TYPE + 1] = {0};
    uint32_t code_off = 0, code_size = 0;
    uint32_t data_off = 0, data_size = 0;
    uint32_t bss_size = 0;
    uint32_t imports_off = 0, imports_size = 0;
    int      has_code = 0;

    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *sec = base + section_table_off + (size_t)i * CVM_SECTION_SIZE;
        uint32_t type     = read_u32_le(sec);
        uint32_t file_off = read_u32_le(sec + 4);
        uint32_t size     = read_u32_le(sec + 8);
        uint32_t sflags   = read_u32_le(sec + 12);

        if (sflags != 0)                          return CVM_E_BAD_SECTION;
        if (type == 0 || type > CVM_MAX_SEC_TYPE) return CVM_E_BAD_SECTION;
        if (type != CVM_SEC_DEBUG) {
            if (seen[type]) return CVM_E_DUP_SECTION;
            seen[type] = 1;
        }

        if (type != CVM_SEC_BSS) {
            if ((uint64_t)file_off + size > len) return CVM_E_TRUNCATED;
        } else if (file_off != 0) {
            return CVM_E_BAD_SECTION;
        }

        switch (type) {
        case CVM_SEC_CODE:
            if (size == 0 || size % 4u != 0) return CVM_E_BAD_SECTION;
            code_off = file_off;
            code_size = size;
            has_code = 1;
            break;
        case CVM_SEC_DATA:
            data_off = file_off;
            data_size = size;
            break;
        case CVM_SEC_BSS:
            bss_size = size;
            break;
        case CVM_SEC_IMPORTS:
            imports_off = file_off;
            imports_size = size;
            break;
        case CVM_SEC_DEBUG:
        default:
            break;
        }
    }

    if (!has_code) return CVM_E_NO_CODE;

    uint32_t code_count = code_size / 4u;
    if (entry >= code_count) return CVM_E_BAD_ENTRY;

    uint64_t heap_total = (uint64_t)data_size + (uint64_t)bss_size;
    if (heap_total > 0xFFFFFFFFu) return CVM_E_BAD_SECTION;

    uint32_t *code = (uint32_t *)malloc((size_t)code_size);
    uint8_t  *heap = NULL;
    void     *imports = NULL;
    if (!code) goto oom;

    if (heap_total > 0) {
        heap = (uint8_t *)malloc((size_t)heap_total);
        if (!heap) goto oom;
    }
    if (imports_size > 0) {
        imports = malloc((size_t)imports_size);
        if (!imports) goto oom;
    }

    for (uint32_t i = 0; i < code_count; ++i)
        code[i] = read_u32_le(base + code_off + (size_t)i * 4u);

    if (data_size) memcpy(heap, base + data_off, data_size);
    if (bss_size)  memset(heap + data_size, 0, bss_size);
    if (imports_size) memcpy(imports, base + imports_off, imports_size);

    out->code         = code;
    out->code_count   = code_count;
    out->heap         = heap;
    out->heap_size    = (uint32_t)heap_total;
    out->data_size    = data_size;
    out->entry        = entry;
    out->imports      = imports;
    out->imports_size = imports_size;
    return CVM_OK;

oom:
    free(code);
    free(heap);
    free(imports);
    return CVM_E_NOMEM;
}

void cvm_image_free(struct cvm_image *img) {
    if (!img) return;
    free(img->code);
    free(img->heap);
    free(img->imports);
    memset(img, 0, sizeof(*img));
}

const char *cvm_strerror(int result) {
    switch (result) {
    case CVM_OK:            return "ok";
    case CVM_E_TRUNCATED:   return "file truncated";
    case CVM_E_BAD_MAGIC:   return "bad magic";
    case CVM_E_BAD_VERSION: return "unsupported version";
    case CVM_E_BAD_SECTION: return "malformed section";
    case CVM_E_DUP_SECTION: return "duplicate section";
    case CVM_E_NO_CODE:     return "missing code section";
    case CVM_E_BAD_ENTRY:   return "entry out of range";
    case CVM_E_NOMEM:       return "out of memory";
    case CVM_E_BAD_OPCODE:  return "unknown opcode";
    case CVM_E_BAD_PC:      return "pc out of range";
    case CVM_E_BAD_ADDR:    return "memory access out of bounds";
    default:                return "unknown error";
    }
}

/* --- Interpreter --------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#  define CVM_THREADED 1
#else
#  define CVM_THREADED 0
#endif

#if CVM_THREADED
#  if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wpedantic"
#    pragma clang diagnostic ignored "-Wgnu-label-as-value"
#    pragma clang diagnostic ignored "-Wgnu-designator"
#  elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
#  endif
#endif

int cvm_run(struct cvm_image *img, int32_t *return_value) {
    if (!img || !img->code || img->code_count == 0) return CVM_E_BAD_PC;
    if (img->entry >= img->code_count)              return CVM_E_BAD_ENTRY;

    int32_t  R[CVM_REG_COUNT];
    memset(R, 0, sizeof(R));

    const uint32_t *code       = img->code;
    const uint32_t  code_count = img->code_count;
    uint8_t        *heap       = img->heap;
    const uint32_t  heap_size  = img->heap_size;
    uint32_t        pc         = img->entry;

    uint32_t inst;
    uint8_t  op, a, b, c;

#if CVM_THREADED
    static const void *const jt[256] = {
        [CVM_OP_HALT] = &&L_HALT,
        [CVM_OP_MOVI] = &&L_MOVI,
        [CVM_OP_MOV]  = &&L_MOV,
        [CVM_OP_ADD]  = &&L_ADD,
        [CVM_OP_SUB]  = &&L_SUB,
        [CVM_OP_MUL]  = &&L_MUL,
        [CVM_OP_LDW]  = &&L_LDW,
        [CVM_OP_STW]  = &&L_STW,
        [CVM_OP_JMP]  = &&L_JMP,
        [CVM_OP_BEQ]  = &&L_BEQ,
        [CVM_OP_BNE]  = &&L_BNE,
    };

#  define DISPATCH() do {                                  \
        if (pc >= code_count) return CVM_E_BAD_PC;         \
        inst = code[pc++];                                 \
        op = (uint8_t)(inst & 0xFFu);                      \
        a  = (uint8_t)((inst >>  8) & 0xFFu);              \
        b  = (uint8_t)((inst >> 16) & 0xFFu);              \
        c  = (uint8_t)((inst >> 24) & 0xFFu);              \
        if (!jt[op]) return CVM_E_BAD_OPCODE;              \
        goto *jt[op];                                      \
    } while (0)

    DISPATCH();

    L_HALT:
        if (return_value) *return_value = R[a];
        return CVM_OK;
    L_MOVI:
        R[a] = (int32_t)(int16_t)(uint16_t)(inst >> 16);
        DISPATCH();
    L_MOV:
        R[a] = R[b];
        DISPATCH();
    L_ADD:
        R[a] = (int32_t)((uint32_t)R[b] + (uint32_t)R[c]);
        DISPATCH();
    L_SUB:
        R[a] = (int32_t)((uint32_t)R[b] - (uint32_t)R[c]);
        DISPATCH();
    L_MUL:
        R[a] = (int32_t)((uint32_t)R[b] * (uint32_t)R[c]);
        DISPATCH();
    L_LDW: {
        uint32_t addr = (uint32_t)R[b];
        if (addr > heap_size || heap_size - addr < 4u) return CVM_E_BAD_ADDR;
        int32_t v;
        memcpy(&v, heap + addr, 4);
        R[a] = v;
        DISPATCH();
    }
    L_STW: {
        uint32_t addr = (uint32_t)R[b];
        if (addr > heap_size || heap_size - addr < 4u) return CVM_E_BAD_ADDR;
        int32_t v = R[c];
        memcpy(heap + addr, &v, 4);
        DISPATCH();
    }
    L_JMP: {
        int32_t off = (int32_t)((inst >> 8) & 0xFFFFFFu);
        if (off & 0x800000) off -= 0x1000000;
        pc = (uint32_t)((int32_t)pc + off);
        DISPATCH();
    }
    L_BEQ:
        if (R[a] == R[b]) pc = (uint32_t)((int32_t)pc + (int32_t)(int8_t)c);
        DISPATCH();
    L_BNE:
        if (R[a] != R[b]) pc = (uint32_t)((int32_t)pc + (int32_t)(int8_t)c);
        DISPATCH();

#  undef DISPATCH

#else  /* !CVM_THREADED — switch fallback for MSVC */

    for (;;) {
        if (pc >= code_count) return CVM_E_BAD_PC;
        inst = code[pc++];
        op = (uint8_t)(inst & 0xFFu);
        a  = (uint8_t)((inst >>  8) & 0xFFu);
        b  = (uint8_t)((inst >> 16) & 0xFFu);
        c  = (uint8_t)((inst >> 24) & 0xFFu);
        switch (op) {
        case CVM_OP_HALT:
            if (return_value) *return_value = R[a];
            return CVM_OK;
        case CVM_OP_MOVI:
            R[a] = (int32_t)(int16_t)(uint16_t)(inst >> 16);
            break;
        case CVM_OP_MOV:  R[a] = R[b]; break;
        case CVM_OP_ADD:  R[a] = (int32_t)((uint32_t)R[b] + (uint32_t)R[c]); break;
        case CVM_OP_SUB:  R[a] = (int32_t)((uint32_t)R[b] - (uint32_t)R[c]); break;
        case CVM_OP_MUL:  R[a] = (int32_t)((uint32_t)R[b] * (uint32_t)R[c]); break;
        case CVM_OP_LDW: {
            uint32_t addr = (uint32_t)R[b];
            if (addr > heap_size || heap_size - addr < 4u) return CVM_E_BAD_ADDR;
            int32_t v;
            memcpy(&v, heap + addr, 4);
            R[a] = v;
            break;
        }
        case CVM_OP_STW: {
            uint32_t addr = (uint32_t)R[b];
            if (addr > heap_size || heap_size - addr < 4u) return CVM_E_BAD_ADDR;
            int32_t v = R[c];
            memcpy(heap + addr, &v, 4);
            break;
        }
        case CVM_OP_JMP: {
            int32_t off = (int32_t)((inst >> 8) & 0xFFFFFFu);
            if (off & 0x800000) off -= 0x1000000;
            pc = (uint32_t)((int32_t)pc + off);
            break;
        }
        case CVM_OP_BEQ:
            if (R[a] == R[b]) pc = (uint32_t)((int32_t)pc + (int32_t)(int8_t)c);
            break;
        case CVM_OP_BNE:
            if (R[a] != R[b]) pc = (uint32_t)((int32_t)pc + (int32_t)(int8_t)c);
            break;
        default:
            return CVM_E_BAD_OPCODE;
        }
    }
#endif
}

#if CVM_THREADED
#  if defined(__clang__)
#    pragma clang diagnostic pop
#  elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#  endif
#endif
