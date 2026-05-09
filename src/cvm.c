#include "cvm.h"

#include <stdlib.h>
#include <string.h>

#define CVM_HEADER_SIZE   24u
#define CVM_SECTION_SIZE  16u
#define CVM_MAX_SEC_TYPE  6u

static uint32_t read_u32_le(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* --- Built-in syscalls --------------------------------------------------- */

static int builtin_sys_heap_start(struct cvm_image *img,
                                  int32_t *regs, void *ud)
{
    (void)ud;
    regs[0] = (int32_t)(img->heap_size - img->reserve_size);
    return 0;
}

static int builtin_sys_heap_size(struct cvm_image *img,
                                 int32_t *regs, void *ud)
{
    (void)ud;
    regs[0] = (int32_t)img->reserve_size;
    return 0;
}

static const struct {
    const char    *name;
    cvm_syscall_fn fn;
} BUILTIN_SYSCALLS[] = {
    { "cvm_sys_heap_start", builtin_sys_heap_start },
    { "cvm_sys_heap_size",  builtin_sys_heap_size  },
};

static void auto_bind_builtins(struct cvm_image *img) {
    for (uint32_t i = 0; i < img->import_count; ++i) {
        for (size_t j = 0;
             j < sizeof BUILTIN_SYSCALLS / sizeof BUILTIN_SYSCALLS[0]; ++j)
        {
            if (strcmp(img->import_names[i], BUILTIN_SYSCALLS[j].name) == 0) {
                img->import_fns[i]      = BUILTIN_SYSCALLS[j].fn;
                img->import_userdata[i] = NULL;
                break;
            }
        }
    }
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
    uint32_t reserve_size = 0;
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

        if (type != CVM_SEC_BSS && type != CVM_SEC_HEAP_RESERVE) {
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
        case CVM_SEC_HEAP_RESERVE:
            reserve_size = size;
            break;
        case CVM_SEC_DEBUG:
        default:
            break;
        }
    }

    if (!has_code) return CVM_E_NO_CODE;

    uint32_t code_count = code_size / 4u;
    if (entry >= code_count) return CVM_E_BAD_ENTRY;

    uint64_t heap_total = (uint64_t)data_size
                        + (uint64_t)bss_size
                        + (uint64_t)reserve_size;
    if (heap_total > 0xFFFFFFFFu) return CVM_E_BAD_SECTION;

    /* Pre-validate IMPORTS layout before any allocation. */
    uint32_t import_count = 0;
    if (imports_size > 0) {
        if (imports_size < 4) return CVM_E_BAD_IMPORTS;
        const uint8_t *ip = base + imports_off;
        import_count = read_u32_le(ip);
        if (import_count > 0xFFFFu) return CVM_E_BAD_IMPORTS;
        uint64_t entries_end = 4u + (uint64_t)import_count * 4u;
        if (entries_end > imports_size) return CVM_E_BAD_IMPORTS;
        for (uint32_t i = 0; i < import_count; ++i) {
            uint32_t name_off = read_u32_le(ip + 4u + (size_t)i * 4u);
            if (name_off >= imports_size) return CVM_E_BAD_IMPORTS;
            if (memchr(ip + name_off, 0, imports_size - name_off) == NULL)
                return CVM_E_BAD_IMPORTS;
        }
    }

    uint32_t       *code         = (uint32_t *)malloc((size_t)code_size);
    uint8_t        *heap         = NULL;
    char           *import_blob  = NULL;
    char          **import_names = NULL;
    cvm_syscall_fn *import_fns   = NULL;
    void          **import_ud    = NULL;
    if (!code) goto oom;

    if (heap_total > 0) {
        heap = (uint8_t *)malloc((size_t)heap_total);
        if (!heap) goto oom;
    }
    if (imports_size > 0) {
        import_blob = (char *)malloc((size_t)imports_size);
        if (!import_blob) goto oom;
        if (import_count > 0) {
            import_names = (char **)calloc(import_count, sizeof(char *));
            import_fns   = (cvm_syscall_fn *)calloc(import_count, sizeof(cvm_syscall_fn));
            import_ud    = (void **)calloc(import_count, sizeof(void *));
            if (!import_names || !import_fns || !import_ud) goto oom;
        }
    }

    for (uint32_t i = 0; i < code_count; ++i)
        code[i] = read_u32_le(base + code_off + (size_t)i * 4u);

    if (data_size) memcpy(heap, base + data_off, data_size);
    if (bss_size + reserve_size)
        memset(heap + data_size, 0, (size_t)bss_size + reserve_size);

    if (imports_size > 0) {
        memcpy(import_blob, base + imports_off, imports_size);
        for (uint32_t i = 0; i < import_count; ++i) {
            uint32_t name_off = read_u32_le((uint8_t *)import_blob + 4u + (size_t)i * 4u);
            import_names[i] = import_blob + name_off;
        }
    }

    out->code             = code;
    out->code_count       = code_count;
    out->heap             = heap;
    out->heap_size        = (uint32_t)heap_total;
    out->data_size        = data_size;
    out->reserve_size     = reserve_size;
    out->entry            = entry;
    out->import_count     = import_count;
    out->import_names     = import_names;
    out->import_fns       = import_fns;
    out->import_userdata  = import_ud;
    out->_import_blob     = import_blob;
    auto_bind_builtins(out);
    return CVM_OK;

oom:
    free(code);
    free(heap);
    free(import_blob);
    free(import_names);
    free(import_fns);
    free(import_ud);
    return CVM_E_NOMEM;
}

void cvm_image_free(struct cvm_image *img) {
    if (!img) return;
    free(img->code);
    free(img->heap);
    free(img->import_names);
    free(img->import_fns);
    free(img->import_userdata);
    free(img->_import_blob);
    memset(img, 0, sizeof(*img));
}

int cvm_link(struct cvm_image *img, const char *name,
             cvm_syscall_fn fn, void *user_data)
{
    if (!img || !name) return CVM_E_NO_SUCH_IMPORT;
    for (uint32_t i = 0; i < img->import_count; ++i) {
        if (strcmp(img->import_names[i], name) == 0) {
            img->import_fns[i]      = fn;
            img->import_userdata[i] = user_data;
            return CVM_OK;
        }
    }
    return CVM_E_NO_SUCH_IMPORT;
}

int cvm_heap_read(struct cvm_image *img, uint32_t addr, void *out, size_t n) {
    if (!img || !out) return CVM_E_BAD_ADDR;
    if (addr > img->heap_size || (size_t)(img->heap_size - addr) < n)
        return CVM_E_BAD_ADDR;
    memcpy(out, img->heap + addr, n);
    return CVM_OK;
}

int cvm_heap_write(struct cvm_image *img, uint32_t addr, const void *in, size_t n) {
    if (!img || !in) return CVM_E_BAD_ADDR;
    if (addr > img->heap_size || (size_t)(img->heap_size - addr) < n)
        return CVM_E_BAD_ADDR;
    memcpy(img->heap + addr, in, n);
    return CVM_OK;
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
    case CVM_E_BAD_OPCODE:       return "unknown opcode";
    case CVM_E_BAD_PC:           return "pc out of range";
    case CVM_E_BAD_ADDR:         return "memory access out of bounds";
    case CVM_E_BAD_IMPORTS:      return "malformed imports section";
    case CVM_E_NO_SUCH_IMPORT:   return "no import with that name";
    case CVM_E_BAD_SYSCALL:      return "syscall id out of range";
    case CVM_E_UNLINKED_SYSCALL: return "syscall has no host handler";
    case CVM_E_SYSCALL_TRAP:     return "syscall handler returned a trap";
    case CVM_E_DIV_BY_ZERO:      return "division by zero";
    default:                     return "unknown error";
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
    return cvm_run_args(img, NULL, 0, return_value);
}

int cvm_run_args(struct cvm_image *img,
                 const int32_t *args, uint32_t arg_count,
                 int32_t *return_value)
{
    if (!img || !img->code || img->code_count == 0) return CVM_E_BAD_PC;
    if (img->entry >= img->code_count)              return CVM_E_BAD_ENTRY;
    if (arg_count > CVM_REG_COUNT)                  return CVM_E_BAD_ADDR;

    int32_t  R[CVM_REG_COUNT];
    memset(R, 0, sizeof(R));
    if (args && arg_count > 0)
        memcpy(R, args, arg_count * sizeof(int32_t));

    const uint32_t *code       = img->code;
    const uint32_t  code_count = img->code_count;
    uint8_t        *heap       = img->heap;
    const uint32_t  heap_size  = img->heap_size;
    uint32_t        pc         = img->entry;

    uint32_t inst;
    uint8_t  op, a, b, c;

#if CVM_THREADED
    static const void *const jt[256] = {
        [CVM_OP_HALT]    = &&L_HALT,
        [CVM_OP_MOVI]    = &&L_MOVI,
        [CVM_OP_MOV]     = &&L_MOV,
        [CVM_OP_ADD]     = &&L_ADD,
        [CVM_OP_SUB]     = &&L_SUB,
        [CVM_OP_MUL]     = &&L_MUL,
        [CVM_OP_LDW]     = &&L_LDW,
        [CVM_OP_STW]     = &&L_STW,
        [CVM_OP_JMP]     = &&L_JMP,
        [CVM_OP_BEQ]     = &&L_BEQ,
        [CVM_OP_BNE]     = &&L_BNE,
        [CVM_OP_SYSCALL] = &&L_SYSCALL,
        [CVM_OP_CMP_EQ]  = &&L_CMP_EQ,
        [CVM_OP_CMP_NE]  = &&L_CMP_NE,
        [CVM_OP_CMP_LT]  = &&L_CMP_LT,
        [CVM_OP_CMP_LE]  = &&L_CMP_LE,
        [CVM_OP_CMP_LTU] = &&L_CMP_LTU,
        [CVM_OP_CMP_LEU] = &&L_CMP_LEU,
        [CVM_OP_DIV]     = &&L_DIV,
        [CVM_OP_DIVU]    = &&L_DIVU,
        [CVM_OP_MOD]     = &&L_MOD,
        [CVM_OP_MODU]    = &&L_MODU,
        [CVM_OP_SHL]     = &&L_SHL,
        [CVM_OP_SHR]     = &&L_SHR,
        [CVM_OP_SAR]     = &&L_SAR,
        [CVM_OP_AND]     = &&L_AND,
        [CVM_OP_OR]      = &&L_OR,
        [CVM_OP_XOR]     = &&L_XOR,
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
    L_SYSCALL: {
        uint32_t id = (uint32_t)(uint16_t)(inst >> 16);
        if (id >= img->import_count)        return CVM_E_BAD_SYSCALL;
        cvm_syscall_fn fn = img->import_fns[id];
        if (!fn)                            return CVM_E_UNLINKED_SYSCALL;
        if (fn(img, R, img->import_userdata[id]) != 0)
            return CVM_E_SYSCALL_TRAP;
        DISPATCH();
    }
    L_CMP_EQ:  R[a] = (R[b] == R[c]) ? 1 : 0; DISPATCH();
    L_CMP_NE:  R[a] = (R[b] != R[c]) ? 1 : 0; DISPATCH();
    L_CMP_LT:  R[a] = (R[b] <  R[c]) ? 1 : 0; DISPATCH();
    L_CMP_LE:  R[a] = (R[b] <= R[c]) ? 1 : 0; DISPATCH();
    L_CMP_LTU: R[a] = ((uint32_t)R[b] <  (uint32_t)R[c]) ? 1 : 0; DISPATCH();
    L_CMP_LEU: R[a] = ((uint32_t)R[b] <= (uint32_t)R[c]) ? 1 : 0; DISPATCH();
    L_DIV: {
        if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
        if (R[b] == INT32_MIN && R[c] == -1) R[a] = INT32_MIN;
        else                                 R[a] = R[b] / R[c];
        DISPATCH();
    }
    L_DIVU: {
        if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
        R[a] = (int32_t)((uint32_t)R[b] / (uint32_t)R[c]);
        DISPATCH();
    }
    L_MOD: {
        if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
        if (R[b] == INT32_MIN && R[c] == -1) R[a] = 0;
        else                                 R[a] = R[b] % R[c];
        DISPATCH();
    }
    L_MODU: {
        if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
        R[a] = (int32_t)((uint32_t)R[b] % (uint32_t)R[c]);
        DISPATCH();
    }
    L_SHL: R[a] = (int32_t)((uint32_t)R[b] << (R[c] & 31)); DISPATCH();
    L_SHR: R[a] = (int32_t)((uint32_t)R[b] >> (R[c] & 31)); DISPATCH();
    L_SAR: R[a] = R[b] >> (R[c] & 31);                      DISPATCH();
    L_AND: R[a] = R[b] & R[c]; DISPATCH();
    L_OR:  R[a] = R[b] | R[c]; DISPATCH();
    L_XOR: R[a] = R[b] ^ R[c]; DISPATCH();

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
        case CVM_OP_SYSCALL: {
            uint32_t id = (uint32_t)(uint16_t)(inst >> 16);
            if (id >= img->import_count)        return CVM_E_BAD_SYSCALL;
            cvm_syscall_fn fn = img->import_fns[id];
            if (!fn)                            return CVM_E_UNLINKED_SYSCALL;
            if (fn(img, R, img->import_userdata[id]) != 0)
                return CVM_E_SYSCALL_TRAP;
            break;
        }
        case CVM_OP_CMP_EQ:  R[a] = (R[b] == R[c]) ? 1 : 0; break;
        case CVM_OP_CMP_NE:  R[a] = (R[b] != R[c]) ? 1 : 0; break;
        case CVM_OP_CMP_LT:  R[a] = (R[b] <  R[c]) ? 1 : 0; break;
        case CVM_OP_CMP_LE:  R[a] = (R[b] <= R[c]) ? 1 : 0; break;
        case CVM_OP_CMP_LTU: R[a] = ((uint32_t)R[b] <  (uint32_t)R[c]) ? 1 : 0; break;
        case CVM_OP_CMP_LEU: R[a] = ((uint32_t)R[b] <= (uint32_t)R[c]) ? 1 : 0; break;
        case CVM_OP_DIV:
            if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
            if (R[b] == INT32_MIN && R[c] == -1) R[a] = INT32_MIN;
            else                                 R[a] = R[b] / R[c];
            break;
        case CVM_OP_DIVU:
            if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
            R[a] = (int32_t)((uint32_t)R[b] / (uint32_t)R[c]);
            break;
        case CVM_OP_MOD:
            if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
            if (R[b] == INT32_MIN && R[c] == -1) R[a] = 0;
            else                                 R[a] = R[b] % R[c];
            break;
        case CVM_OP_MODU:
            if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
            R[a] = (int32_t)((uint32_t)R[b] % (uint32_t)R[c]);
            break;
        case CVM_OP_SHL: R[a] = (int32_t)((uint32_t)R[b] << (R[c] & 31)); break;
        case CVM_OP_SHR: R[a] = (int32_t)((uint32_t)R[b] >> (R[c] & 31)); break;
        case CVM_OP_SAR: R[a] = R[b] >> (R[c] & 31); break;
        case CVM_OP_AND: R[a] = R[b] & R[c]; break;
        case CVM_OP_OR:  R[a] = R[b] | R[c]; break;
        case CVM_OP_XOR: R[a] = R[b] ^ R[c]; break;
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
