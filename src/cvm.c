#include "cvm.h"

#include <math.h>     /* sqrtf() — used by CVM_OP_FSQRT. On hosts without
                         a hardware FPU this resolves to libm/libgcc soft
                         implementation, same trade-off as the FADD/FMUL
                         family already documented for soft-float targets. */
#include <string.h>

/* Embedded targets that want to forbid the libc malloc/free path entirely
 * (and avoid linking <stdlib.h>) can build with -DCVM_NO_STDLIB_FALLBACK.
 * In that mode a NULL allocator is hard-fail: cvm_int_alloc returns NULL,
 * cvm_int_free is a no-op, and callers must supply a non-NULL
 * cvm_allocator_t with both function pointers populated. */
#ifndef CVM_NO_STDLIB_FALLBACK
#  include <stdlib.h>
#endif

#define CVM_HEADER_SIZE   24u
#define CVM_SECTION_SIZE  16u
#define CVM_MAX_SEC_TYPE  12u   /* includes CVM_SEC_META/SEAL (host-only, ignored by loader) */
#define CVM_REGION_ENTRY_SIZE 28u   /* name[16] + size + direction + flags */

/* --- Optional self-time profiler (build with -DCVM_PROFILE) ---------------
 * When enabled, the interpreter attributes every executed instruction to the
 * currently-running FUNCS index (tracked with a shadow call stack alongside
 * the real one). cvm_prof_counts[i] is the self-instruction count for func i
 * — exclusive of callees, so it points at where the interpreter actually
 * spends cycles. Zero overhead when CVM_PROFILE is undefined. */
#ifdef CVM_PROFILE
#include <stdlib.h>
uint64_t *cvm_prof_counts = NULL;   /* [cvm_prof_len] self-instruction counts */
uint32_t  cvm_prof_len    = 0;
uint64_t  cvm_prof_total  = 0;       /* total instructions executed           */
/* Caller attribution: when cvm_prof_watch names a FUNCS index, every CALL/
 * CALLR to it bumps cvm_prof_caller[caller], so the host can see who drives a
 * hot leaf. Off (no attribution) when watch == 0xFFFFFFFF. */
uint32_t  cvm_prof_watch  = 0xFFFFFFFFu;
uint64_t *cvm_prof_caller = NULL;   /* [cvm_prof_len] calls into the watched fn */

void cvm_profile_reset(uint32_t func_count) {
    uint32_t n = func_count ? func_count : 1u;
    free(cvm_prof_counts);
    free(cvm_prof_caller);
    cvm_prof_counts = (uint64_t *)calloc(n, sizeof(uint64_t));
    cvm_prof_caller = (uint64_t *)calloc(n, sizeof(uint64_t));
    cvm_prof_len    = cvm_prof_counts ? func_count : 0u;
    cvm_prof_total  = 0;
}
#endif

/* PROF_TICK charges the instruction about to run to the current function;
 * PROF_CALL/PROF_RET keep the shadow call stack in step with CALL/CALLR/RET.
 * All three vanish when CVM_PROFILE is undefined. They reference locals
 * declared inside cvm_exec_at, so they only ever expand there. */
#ifdef CVM_PROFILE
#  define PROF_TICK()    do { if (prof_cur < cvm_prof_len) cvm_prof_counts[prof_cur]++; \
                              cvm_prof_total++; } while (0)
#  define PROF_CALL(fid) do { if (prof_sp < CVM_PROF_DEPTH) prof_stack[prof_sp++] = prof_cur; \
                              if ((uint32_t)(fid) == cvm_prof_watch && prof_cur < cvm_prof_len) \
                                  cvm_prof_caller[prof_cur]++; \
                              prof_cur = (uint32_t)(fid); } while (0)
#  define PROF_RET()     do { if (prof_sp > 0) prof_cur = prof_stack[--prof_sp]; } while (0)
#else
#  define PROF_TICK()    ((void)0)
#  define PROF_CALL(fid) ((void)0)
#  define PROF_RET()     ((void)0)
#endif

static uint32_t read_u32_le(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* --- Allocator routing --------------------------------------------------- *
 * Internal helpers that fall through to stdlib when the allocator (or
 * its individual function pointers) is NULL. The load path threads a
 * `cvm_allocator_t *` around; cvm_image_free recovers the same hooks
 * by passing &img->allocator. Either function pointer being NULL is a
 * legal "use stdlib for this slot" signal — unless built with
 * CVM_NO_STDLIB_FALLBACK, in which case a missing hook is hard-fail
 * (alloc returns NULL, free is a no-op). */

static void *cvm_int_alloc(const cvm_allocator_t *a, size_t n) {
    if (n == 0) return NULL;
    if (a && a->alloc_fn) return a->alloc_fn(n, a->user_data);
#ifdef CVM_NO_STDLIB_FALLBACK
    return NULL;
#else
    return malloc(n);
#endif
}

static void cvm_int_free(const cvm_allocator_t *a, void *p) {
    if (!p) return;
    if (a && a->free_fn) { a->free_fn(p, a->user_data); return; }
#ifndef CVM_NO_STDLIB_FALLBACK
    free(p);
#endif
}

static void *cvm_int_calloc(const cvm_allocator_t *a,
                            size_t n_elem, size_t elem_size)
{
    if (n_elem != 0 && elem_size > (size_t)(-1) / n_elem) return NULL;
    size_t total = n_elem * elem_size;
    void  *p     = cvm_int_alloc(a, total);
    if (p) memset(p, 0, total);
    return p;
}

/* --- Library version ----------------------------------------------------- */

const char *cvm_version_string(void) { return CVM_VERSION_STRING; }

uint32_t cvm_version_number(void) {
    return ((uint32_t)CVM_VERSION_MAJOR << 16)
         | ((uint32_t)CVM_VERSION_MINOR <<  8)
         |  (uint32_t)CVM_VERSION_PATCH;
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

/* cvm_sys_get_region(name_addr) — name_addr is a heap address pointing at a
 * NUL-terminated string. Returns the region's offset on hit, or -1 on miss
 * (the caller can test for a negative return; offset 0 can never name a
 * region since regions always sit after DATA+BSS). The string is read from
 * VM memory with bounds checks, so a malicious VM can't escape the heap. */
static int builtin_sys_get_region(struct cvm_image *img,
                                  int32_t *regs, void *ud)
{
    (void)ud;
    uint32_t addr = (uint32_t)regs[0];
    if (addr >= img->heap_size) { regs[0] = -1; return 0; }
    /* Find a NUL within reach. Cap at 16 since region names are at most 16
     * bytes (NUL included); a longer string can't possibly match anyway. */
    uint32_t maxlen = img->heap_size - addr;
    if (maxlen > 16u) maxlen = 16u;
    const char *s = (const char *)(img->heap + addr);
    uint32_t nlen = 0;
    while (nlen < maxlen && s[nlen] != '\0') ++nlen;
    if (nlen == maxlen) { regs[0] = -1; return 0; }     /* no NUL in 16 bytes */
    for (uint32_t i = 0; i < img->region_count; ++i) {
        if (strncmp(img->regions[i].name, s, 16) == 0
         && img->regions[i].name[nlen] == '\0')
        {
            regs[0] = (int32_t)img->regions[i].offset;
            return 0;
        }
    }
    regs[0] = -1;
    return 0;
}

/* cvm_sys_rom_base() — heap offset of the read-only cartridge ROM, or 0 if
 * the binary carries none. cvm_sys_rom_size() — its length in bytes (0 when
 * absent). A program tests size first, then treats base as a pointer. */
static int builtin_sys_rom_base(struct cvm_image *img,
                                int32_t *regs, void *ud)
{
    (void)ud;
    regs[0] = (int32_t)img->rom_offset;
    return 0;
}

static int builtin_sys_rom_size(struct cvm_image *img,
                                int32_t *regs, void *ud)
{
    (void)ud;
    regs[0] = (int32_t)img->rom_size;
    return 0;
}

static const struct {
    const char    *name;
    cvm_syscall_fn fn;
} BUILTIN_SYSCALLS[] = {
    { "cvm_sys_heap_start", builtin_sys_heap_start },
    { "cvm_sys_heap_size",  builtin_sys_heap_size  },
    { "cvm_sys_get_region", builtin_sys_get_region },
    { "cvm_sys_rom_base",   builtin_sys_rom_base   },
    { "cvm_sys_rom_size",   builtin_sys_rom_size   },
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
    return cvm_load_ex(bytes, len, out, NULL);
}

int cvm_peek_section(const void *bytes, size_t len, enum cvm_section_type type,
                     const unsigned char **out_ptr, uint32_t *out_size)
{
    if (out_ptr)  *out_ptr  = NULL;
    if (out_size) *out_size = 0;
    if (!bytes || len < CVM_HEADER_SIZE) return -1;

    const uint8_t *base = (const uint8_t *)bytes;
    if (base[0] != 'C' || base[1] != 'V' || base[2] != 'M' || base[3] != '1')
        return -1;

    uint32_t section_count     = read_u32_le(base + 12);
    uint32_t section_table_off = read_u32_le(base + 16);
    uint64_t table_end = (uint64_t)section_table_off
                       + (uint64_t)section_count * CVM_SECTION_SIZE;
    if (table_end > len) return -1;

    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t *sec = base + section_table_off + (size_t)i * CVM_SECTION_SIZE;
        uint32_t t  = read_u32_le(sec);
        uint32_t fo = read_u32_le(sec + 4);
        uint32_t sz = read_u32_le(sec + 8);
        if (t != (uint32_t)type) continue;
        if ((uint64_t)fo + sz > len) return -1;
        if (out_ptr)  *out_ptr  = base + fo;
        if (out_size) *out_size = sz;
        return 1;
    }
    return 0;
}

uint32_t cvm_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

int cvm_seal_check(const void *bytes, size_t len) {
    const unsigned char *seal = NULL;
    uint32_t sz = 0;
    int r = cvm_peek_section(bytes, len, CVM_SEC_SEAL, &seal, &sz);
    if (r != 1) return r < 0 ? -1 : 0;          /* absent (0) or malformed (-1) */
    if (sz < 12 || read_u32_le(seal) != CVM_SEAL_MAGIC) return -1;
    uint32_t stored = read_u32_le(seal + 8);
    size_t   seal_off = (size_t)(seal - (const unsigned char *)bytes);
    return (cvm_crc32(bytes, seal_off) == stored) ? 1 : -1;
}

int cvm_load_ex(const void *bytes, size_t len, struct cvm_image *out,
                const cvm_allocator_t *allocator)
{
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
    uint32_t stack_size = 0;
    uint32_t imports_off = 0, imports_size = 0;
    uint32_t funcs_off = 0, funcs_size = 0;
    uint32_t regions_off = 0, regions_size = 0;
    uint32_t rom_off = 0, rom_size = 0;
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

        int has_payload = (type != CVM_SEC_BSS
                        && type != CVM_SEC_HEAP_RESERVE
                        && type != CVM_SEC_STACK_RESERVE);
        if (has_payload) {
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
        case CVM_SEC_STACK_RESERVE:
            stack_size = size;
            break;
        case CVM_SEC_FUNCS:
            if (size == 0 || size % 4u != 0) return CVM_E_BAD_FUNCS;
            funcs_off  = file_off;
            funcs_size = size;
            break;
        case CVM_SEC_HOST_REGION:
            regions_off  = file_off;
            regions_size = size;
            break;
        case CVM_SEC_ROM:
            rom_off  = file_off;
            rom_size = size;
            break;
        case CVM_SEC_META:   /* host-only metadata; range already validated, not loaded */
        case CVM_SEC_SEAL:   /* host-only integrity seal; verified by cvm_seal_check */
        case CVM_SEC_DEBUG:
        default:
            break;
        }
    }

    if (!has_code) return CVM_E_NO_CODE;

    uint32_t code_count = code_size / 4u;
    if (entry >= code_count) return CVM_E_BAD_ENTRY;

    /* Pre-validate HOST_REGION layout. Each region's payload sits between
     * BSS and HEAP_RESERVE in img->heap. We round each region's size up to
     * 4 so that LDW/STW into a region naturally hit aligned addresses. */
    uint32_t region_count = 0;
    uint64_t region_total = 0;
    if (regions_size > 0) {
        if (regions_size < 4) return CVM_E_BAD_REGION;
        const uint8_t *rp = base + regions_off;
        region_count = read_u32_le(rp);
        if (region_count > 0xFFFFu) return CVM_E_BAD_REGION;
        uint64_t entries_end = 4u + (uint64_t)region_count * CVM_REGION_ENTRY_SIZE;
        if (entries_end > regions_size) return CVM_E_BAD_REGION;
        for (uint32_t i = 0; i < region_count; ++i) {
            const uint8_t *e = rp + 4u + (size_t)i * CVM_REGION_ENTRY_SIZE;
            /* Name field must be NUL-terminated within its 16 bytes. */
            if (memchr(e, 0, 16) == NULL) return CVM_E_BAD_REGION;
            /* First byte == 0 means an empty name; reject. */
            if (e[0] == 0) return CVM_E_BAD_REGION;
            uint32_t rsize = read_u32_le(e + 16);
            uint32_t rdir  = read_u32_le(e + 20);
            uint32_t rflag = read_u32_le(e + 24);
            if (rflag != 0) return CVM_E_BAD_REGION;
            if (rdir < CVM_REGION_R || rdir > CVM_REGION_RW)
                return CVM_E_BAD_REGION;
            /* Reject duplicate names. O(N²) but N is bounded by 65535 in
             * principle; in practice dozens at most. */
            for (uint32_t j = 0; j < i; ++j) {
                const uint8_t *o = rp + 4u + (size_t)j * CVM_REGION_ENTRY_SIZE;
                if (strncmp((const char *)e, (const char *)o, 16) == 0)
                    return CVM_E_BAD_REGION;
            }
            uint64_t aligned = ((uint64_t)rsize + 3u) & ~(uint64_t)3u;
            region_total += aligned;
            if (region_total > 0xFFFFFFFFu) return CVM_E_BAD_REGION;
        }
    }

    /* Layout inside the heap: DATA | BSS | REGIONS | ROM | RESERVE.
     * ROM sits before RESERVE so cvm_sys_heap_start (heap_size - reserve_size)
     * still points at the free region. */
    uint32_t rom_offset = data_size + bss_size + (uint32_t)region_total;
    uint64_t heap_total = (uint64_t)data_size
                        + (uint64_t)bss_size
                        + region_total
                        + (uint64_t)rom_size
                        + (uint64_t)reserve_size;
    if (heap_total > 0xFFFFFFFFu) return CVM_E_BAD_SECTION;

    /* Stack must leave room for at least the sentinel return PC. */
    if (stack_size != 0 && stack_size < 4u) return CVM_E_BAD_SECTION;
    uint64_t mem_total = heap_total + (uint64_t)stack_size;
    if (mem_total > 0xFFFFFFFFu) return CVM_E_BAD_SECTION;

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

    uint32_t          *code         = (uint32_t *)cvm_int_alloc(allocator, (size_t)code_size);
    uint8_t           *heap         = NULL;
    char              *import_blob  = NULL;
    char             **import_names = NULL;
    cvm_syscall_fn    *import_fns   = NULL;
    void             **import_ud    = NULL;
    uint32_t          *func_offsets = NULL;
    uint32_t           func_count   = 0;
    struct cvm_region *regions      = NULL;
    if (!code) goto oom;

    if (mem_total > 0) {
        heap = (uint8_t *)cvm_int_alloc(allocator, (size_t)mem_total);
        if (!heap) goto oom;
    }
    if (funcs_size > 0) {
        func_count   = funcs_size / 4u;
        func_offsets = (uint32_t *)cvm_int_alloc(allocator, funcs_size);
        if (!func_offsets) goto oom;
    }
    if (region_count > 0) {
        regions = (struct cvm_region *)cvm_int_calloc(allocator, region_count,
                                                      sizeof(struct cvm_region));
        if (!regions) goto oom;
    }
    if (imports_size > 0) {
        import_blob = (char *)cvm_int_alloc(allocator, (size_t)imports_size);
        if (!import_blob) goto oom;
        if (import_count > 0) {
            import_names = (char **)cvm_int_calloc(allocator, import_count, sizeof(char *));
            import_fns   = (cvm_syscall_fn *)cvm_int_calloc(allocator, import_count, sizeof(cvm_syscall_fn));
            import_ud    = (void **)cvm_int_calloc(allocator, import_count, sizeof(void *));
            if (!import_names || !import_fns || !import_ud) goto oom;
        }
    }

    for (uint32_t i = 0; i < code_count; ++i)
        code[i] = read_u32_le(base + code_off + (size_t)i * 4u);

    if (data_size) memcpy(heap, base + data_off, data_size);
    if (mem_total > data_size)
        memset(heap + data_size, 0, (size_t)mem_total - (size_t)data_size);

    /* Copy the read-only ROM blob into its slot (after the zero-fill above,
     * which would otherwise wipe it). */
    if (rom_size) memcpy(heap + rom_offset, base + rom_off, rom_size);

    if (func_count > 0) {
        for (uint32_t i = 0; i < func_count; ++i) {
            uint32_t off = read_u32_le(base + funcs_off + (size_t)i * 4u);
            if (off >= code_count) {
                cvm_int_free(allocator, code);
                cvm_int_free(allocator, heap);
                cvm_int_free(allocator, func_offsets);
                cvm_int_free(allocator, regions);
                cvm_int_free(allocator, import_blob);
                cvm_int_free(allocator, import_names);
                cvm_int_free(allocator, import_fns);
                cvm_int_free(allocator, import_ud);
                return CVM_E_BAD_FUNCS;
            }
            func_offsets[i] = off;
        }
    }

    if (imports_size > 0) {
        memcpy(import_blob, base + imports_off, imports_size);
        for (uint32_t i = 0; i < import_count; ++i) {
            uint32_t name_off = read_u32_le((uint8_t *)import_blob + 4u + (size_t)i * 4u);
            import_names[i] = import_blob + name_off;
        }
    }

    if (region_count > 0) {
        const uint8_t *rp = base + regions_off;
        uint32_t cursor = data_size + bss_size;   /* regions sit after BSS */
        for (uint32_t i = 0; i < region_count; ++i) {
            const uint8_t *e = rp + 4u + (size_t)i * CVM_REGION_ENTRY_SIZE;
            memcpy(regions[i].name, e, 16);       /* already validated NUL */
            regions[i].size      = read_u32_le(e + 16);
            regions[i].direction = read_u32_le(e + 20);
            regions[i].offset    = cursor;
            cursor += (regions[i].size + 3u) & ~(uint32_t)3u;
        }
    }

    out->code             = code;
    out->code_count       = code_count;
    out->heap             = heap;
    out->heap_size        = (uint32_t)heap_total;
    out->data_size        = data_size;
    out->reserve_size     = reserve_size;
    out->stack_size       = stack_size;
    out->mem_size         = (uint32_t)mem_total;
    out->entry            = entry;
    out->func_offsets     = func_offsets;
    out->func_count       = func_count;
    out->import_count     = import_count;
    out->import_names     = import_names;
    out->import_fns       = import_fns;
    out->import_userdata  = import_ud;
    out->_import_blob     = import_blob;
    out->regions          = regions;
    out->region_count     = region_count;
    out->rom_offset       = rom_size ? rom_offset : 0;
    out->rom_size         = rom_size;
    /* Stash the allocator so cvm_image_free uses the matching free
     * for every block we just returned. NULL fields fall through to
     * stdlib free, which is what the simple cvm_load case wants. */
    if (allocator) {
        out->allocator.alloc_fn  = allocator->alloc_fn;
        out->allocator.free_fn   = allocator->free_fn;
        out->allocator.user_data = allocator->user_data;
    }
    auto_bind_builtins(out);
    return CVM_OK;

oom:
    cvm_int_free(allocator, code);
    cvm_int_free(allocator, heap);
    cvm_int_free(allocator, import_blob);
    cvm_int_free(allocator, import_names);
    cvm_int_free(allocator, import_fns);
    cvm_int_free(allocator, import_ud);
    cvm_int_free(allocator, func_offsets);
    cvm_int_free(allocator, regions);
    return CVM_E_NOMEM;
}

void cvm_image_free(struct cvm_image *img) {
    if (!img) return;
    /* Reconstruct the allocator handle the load path used. The
     * cvm_int_free helper takes the same struct shape; a NULL
     * function pointer falls through to stdlib free. */
    cvm_allocator_t a = {
        img->allocator.alloc_fn,
        img->allocator.free_fn,
        img->allocator.user_data,
    };
    cvm_int_free(&a, img->code);
    cvm_int_free(&a, img->heap);
    cvm_int_free(&a, img->import_names);
    cvm_int_free(&a, img->import_fns);
    cvm_int_free(&a, img->import_userdata);
    cvm_int_free(&a, img->_import_blob);
    cvm_int_free(&a, img->func_offsets);
    cvm_int_free(&a, img->regions);
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

/* Syscall-facing pointer accessors. They bound-check against mem_size (the
 * full allocation: DATA|BSS|REGIONS|ROM|RESERVE|STACK), matching the in-VM
 * LDB/STB checks above — NOT heap_size, which excludes the stack. Otherwise a
 * cart could read/write a stack-local with LDB/STB but a syscall handed the
 * same pointer (e.g. cron_log of a stack buffer, cron_mouse(&x,&y)) would trap.
 * The stack is the cart's own memory; there is no reason to forbid it here. */
int cvm_heap_read(struct cvm_image *img, uint32_t addr, void *out, size_t n) {
    if (!img || !out) return CVM_E_BAD_ADDR;
    if (addr > img->mem_size || (size_t)(img->mem_size - addr) < n)
        return CVM_E_BAD_ADDR;
    memcpy(out, img->heap + addr, n);
    return CVM_OK;
}

int cvm_heap_write(struct cvm_image *img, uint32_t addr, const void *in, size_t n) {
    if (!img || !in) return CVM_E_BAD_ADDR;
    if (addr > img->mem_size || (size_t)(img->mem_size - addr) < n)
        return CVM_E_BAD_ADDR;
    memcpy(img->heap + addr, in, n);
    return CVM_OK;
}

int cvm_image_get_region(struct cvm_image *img, const char *name,
                         uint32_t *out_offset, uint32_t *out_size)
{
    if (!img || !name) return CVM_E_NO_SUCH_REGION;
    for (uint32_t i = 0; i < img->region_count; ++i) {
        if (strncmp(img->regions[i].name, name, 16) == 0) {
            if (out_offset) *out_offset = img->regions[i].offset;
            if (out_size)   *out_size   = img->regions[i].size;
            return CVM_OK;
        }
    }
    return CVM_E_NO_SUCH_REGION;
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
    case CVM_E_BAD_FUNCS:        return "malformed funcs section";
    case CVM_E_BAD_FUNC_INDEX:   return "call target index out of range";
    case CVM_E_STACK_OVERFLOW:   return "stack overflow";
    case CVM_E_NULL_FUNC_PTR:    return "null function pointer call";
    case CVM_E_BAD_REGION:       return "malformed host_region section";
    case CVM_E_NO_SUCH_REGION:   return "no region with that name";
    default:                     return "unknown error";
    }
}

/* --- Interpreter --------------------------------------------------------- */

/* Bitcast helpers: f32 values share the i32 register file. memcpy is the
 * strict-aliasing-safe way to round-trip the bits, and clang/gcc optimise
 * a 4-byte memcpy into nothing on every target we care about. */
static inline float    cvm_bits_to_f32(int32_t bits) {
    float f; memcpy(&f, &bits, sizeof f); return f;
}
static inline int32_t  cvm_f32_to_bits(float f) {
    int32_t b; memcpy(&b, &f, sizeof b); return b;
}

/* Saturating float→int with NaN→0. Pinned semantics so the same .bin
 * gives the same result on every host (raw `(int32_t)f` is x86 indefinite
 * vs ARM saturating vs RISC-V saturating — we can't trust the C cast).
 * Boundary values: 2147483648.0f is exactly 2^31 so any f >= it overflows
 * INT32_MAX; -2147483648.0f represents INT32_MIN exactly so f < it is
 * underflow. Same logic for the unsigned variant against 4294967296.0f
 * (exactly 2^32) and 0. */
static inline int32_t  cvm_f32_to_i32_sat(float f) {
    if (f != f)                 return 0;
    if (f >=  2147483648.0f)    return INT32_MAX;
    if (f <  -2147483648.0f)    return INT32_MIN;
    return (int32_t)f;
}
static inline uint32_t cvm_f32_to_u32_sat(float f) {
    if (f != f)                 return 0;
    if (f >=  4294967296.0f)    return UINT32_MAX;
    if (f <   0.0f)             return 0;
    return (uint32_t)f;
}

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

static int cvm_exec_at(struct cvm_image *img,
                       uint32_t          start_pc,
                       const int32_t    *args, uint32_t arg_count,
                       int32_t          *return_value);

int cvm_run(struct cvm_image *img, int32_t *return_value) {
    return cvm_run_args(img, NULL, 0, return_value);
}

int cvm_run_args(struct cvm_image *img,
                 const int32_t *args, uint32_t arg_count,
                 int32_t *return_value)
{
    if (!img) return CVM_E_BAD_PC;
    return cvm_exec_at(img, img->entry, args, arg_count, return_value);
}

int cvm_call(struct cvm_image *img,
             uint32_t fn_index,
             const int32_t *args, uint32_t arg_count,
             int32_t *return_value)
{
    if (!img || !img->func_offsets) return CVM_E_BAD_FUNCS;
    if (fn_index == 0)              return CVM_E_NULL_FUNC_PTR;
    if (fn_index >= img->func_count) return CVM_E_BAD_FUNC_INDEX;
    uint32_t start_pc = img->func_offsets[fn_index];
    return cvm_exec_at(img, start_pc, args, arg_count, return_value);
}

/* Internal: run the interpreter starting at `start_pc`. The public
 * entry points (cvm_run / cvm_run_args / cvm_call) are thin shims that
 * pick a start PC and forward here. */
static int cvm_exec_at(struct cvm_image *img,
                       uint32_t          start_pc,
                       const int32_t    *args, uint32_t arg_count,
                       int32_t          *return_value)
{
    if (!img || !img->code || img->code_count == 0) return CVM_E_BAD_PC;
    if (start_pc >= img->code_count)                return CVM_E_BAD_ENTRY;
    if (arg_count > CVM_REG_COUNT)                  return CVM_E_BAD_ADDR;

    int32_t  R[CVM_REG_COUNT];
    memset(R, 0, sizeof(R));
    if (args && arg_count > 0)
        memcpy(R, args, arg_count * sizeof(int32_t));

    const uint32_t *code         = img->code;
    const uint32_t  code_count   = img->code_count;
    uint8_t        *heap         = img->heap;
    const uint32_t  mem_size     = img->mem_size;
    const uint32_t *func_offsets = img->func_offsets;
    const uint32_t  func_count   = img->func_count;
    uint32_t        pc           = start_pc;

#ifdef CVM_PROFILE
#  define CVM_PROF_DEPTH 8192u
    uint32_t prof_stack[CVM_PROF_DEPTH];
    uint32_t prof_sp  = 0;
    uint32_t prof_cur = 0;              /* FUNCS index of start_pc, if known */
    for (uint32_t fi = 1; fi < func_count; ++fi)
        if (func_offsets[fi] == start_pc) { prof_cur = fi; break; }
#endif

    /* Set up SP at the very top of memory and push the run-completion
     * sentinel as the outermost return PC. RET pops it and halts. Programs
     * that don't use CALL/RET get a zero stack region and never touch SP. */
    if (img->stack_size >= 4u) {
        uint32_t sp = mem_size - 4u;
        uint32_t sentinel = CVM_RET_SENTINEL;
        memcpy(heap + sp, &sentinel, 4);
        R[CVM_REG_SP] = (int32_t)sp;
    } else {
        R[CVM_REG_SP] = (int32_t)mem_size;
    }

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
        [CVM_OP_CALL]    = &&L_CALL,
        [CVM_OP_RET]     = &&L_RET,
        [CVM_OP_CALLR]   = &&L_CALLR,
        [CVM_OP_LDB]     = &&L_LDB,
        [CVM_OP_STB]     = &&L_STB,
        [CVM_OP_LDH]     = &&L_LDH,
        [CVM_OP_STH]     = &&L_STH,
        [CVM_OP_MOVHI]   = &&L_MOVHI,
        [CVM_OP_MEMCPY]  = &&L_MEMCPY,
        [CVM_OP_MEMSET]  = &&L_MEMSET,
        [CVM_OP_MEMMOVE] = &&L_MEMMOVE,
        [CVM_OP_MULH]    = &&L_MULH,
        [CVM_OP_MULHU]   = &&L_MULHU,
        [CVM_OP_FADD]    = &&L_FADD,
        [CVM_OP_FSUB]    = &&L_FSUB,
        [CVM_OP_FMUL]    = &&L_FMUL,
        [CVM_OP_FDIV]    = &&L_FDIV,
        [CVM_OP_FNEG]    = &&L_FNEG,
        [CVM_OP_FCMP_EQ] = &&L_FCMP_EQ,
        [CVM_OP_FCMP_NE] = &&L_FCMP_NE,
        [CVM_OP_FCMP_LT] = &&L_FCMP_LT,
        [CVM_OP_FCMP_LE] = &&L_FCMP_LE,
        [CVM_OP_F2I_S]   = &&L_F2I_S,
        [CVM_OP_F2I_U]   = &&L_F2I_U,
        [CVM_OP_I2F_S]   = &&L_I2F_S,
        [CVM_OP_I2F_U]   = &&L_I2F_U,
        [CVM_OP_JMPR]    = &&L_JMPR,
        [CVM_OP_FSQRT]   = &&L_FSQRT,
        [CVM_OP_QDIV1616] = &&L_QDIV1616,
        [CVM_OP_QDIV6432] = &&L_QDIV6432,
        [CVM_OP_SETJMP]   = &&L_SETJMP,
        [CVM_OP_LONGJMP]  = &&L_LONGJMP,
    };

#  define DISPATCH() do {                                  \
        if (pc >= code_count) return CVM_E_BAD_PC;         \
        PROF_TICK();                                       \
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
        if (addr > mem_size || mem_size - addr < 4u) return CVM_E_BAD_ADDR;
        int32_t v;
        memcpy(&v, heap + addr, 4);
        R[a] = v;
        DISPATCH();
    }
    L_STW: {
        uint32_t addr = (uint32_t)R[b];
        if (addr > mem_size || mem_size - addr < 4u) return CVM_E_BAD_ADDR;
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
    L_CALL: {
        uint32_t fid = (inst >> 8) & 0xFFFFFFu;
        if (fid == 0) return CVM_E_NULL_FUNC_PTR;
        if (fid >= func_count) return CVM_E_BAD_FUNC_INDEX;
        uint32_t sp = (uint32_t)R[CVM_REG_SP];
        if (sp < 4u || sp > mem_size) return CVM_E_STACK_OVERFLOW;
        sp -= 4u;
        memcpy(heap + sp, &pc, 4);
        R[CVM_REG_SP] = (int32_t)sp;
        pc = func_offsets[fid];
        PROF_CALL(fid);
        DISPATCH();
    }
    L_RET: {
        uint32_t sp = (uint32_t)R[CVM_REG_SP];
        if (sp > mem_size || mem_size - sp < 4u) return CVM_E_BAD_ADDR;
        uint32_t ret_pc;
        memcpy(&ret_pc, heap + sp, 4);
        R[CVM_REG_SP] = (int32_t)(sp + 4u);
        if (ret_pc == CVM_RET_SENTINEL) {
            if (return_value) *return_value = R[0];
            return CVM_OK;
        }
        pc = ret_pc;
        PROF_RET();
        DISPATCH();
    }
    L_CALLR: {
        uint32_t fid = (uint32_t)R[a];
        if (fid == 0) return CVM_E_NULL_FUNC_PTR;
        if (fid >= func_count) return CVM_E_BAD_FUNC_INDEX;
        uint32_t sp = (uint32_t)R[CVM_REG_SP];
        if (sp < 4u || sp > mem_size) return CVM_E_STACK_OVERFLOW;
        sp -= 4u;
        memcpy(heap + sp, &pc, 4);
        R[CVM_REG_SP] = (int32_t)sp;
        pc = func_offsets[fid];
        PROF_CALL(fid);
        DISPATCH();
    }
    L_LDB: {
        uint32_t addr = (uint32_t)R[b];
        if (addr >= mem_size) return CVM_E_BAD_ADDR;
        R[a] = (int32_t)(uint32_t)heap[addr];
        DISPATCH();
    }
    L_STB: {
        uint32_t addr = (uint32_t)R[b];
        if (addr >= mem_size) return CVM_E_BAD_ADDR;
        heap[addr] = (uint8_t)((uint32_t)R[c] & 0xFFu);
        DISPATCH();
    }
    L_LDH: {
        uint32_t addr = (uint32_t)R[b];
        if (addr > mem_size || mem_size - addr < 2u) return CVM_E_BAD_ADDR;
        uint16_t v;
        memcpy(&v, heap + addr, 2);
        R[a] = (int32_t)(uint32_t)v;
        DISPATCH();
    }
    L_STH: {
        uint32_t addr = (uint32_t)R[b];
        if (addr > mem_size || mem_size - addr < 2u) return CVM_E_BAD_ADDR;
        uint16_t v = (uint16_t)((uint32_t)R[c] & 0xFFFFu);
        memcpy(heap + addr, &v, 2);
        DISPATCH();
    }
    L_MOVHI: {
        uint32_t hi = (inst >> 16) & 0xFFFFu;
        R[a] = (int32_t)((hi << 16) | ((uint32_t)R[a] & 0xFFFFu));
        DISPATCH();
    }
    L_MEMCPY: {
        uint32_t dst = (uint32_t)R[a];
        uint32_t src = (uint32_t)R[b];
        uint32_t len = (uint32_t)R[c];
        if (len) {
            if (dst > mem_size || mem_size - dst < len) return CVM_E_BAD_ADDR;
            if (src > mem_size || mem_size - src < len) return CVM_E_BAD_ADDR;
            memcpy(heap + dst, heap + src, len);
        }
        DISPATCH();
    }
    L_MEMSET: {
        uint32_t dst = (uint32_t)R[a];
        uint32_t len = (uint32_t)R[c];
        if (len) {
            if (dst > mem_size || mem_size - dst < len) return CVM_E_BAD_ADDR;
            memset(heap + dst, (int)((uint32_t)R[b] & 0xFFu), len);
        }
        DISPATCH();
    }
    L_MEMMOVE: {
        uint32_t dst = (uint32_t)R[a];
        uint32_t src = (uint32_t)R[b];
        uint32_t len = (uint32_t)R[c];
        if (len) {
            if (dst > mem_size || mem_size - dst < len) return CVM_E_BAD_ADDR;
            if (src > mem_size || mem_size - src < len) return CVM_E_BAD_ADDR;
            memmove(heap + dst, heap + src, len);
        }
        DISPATCH();
    }
    L_MULH:
        R[a] = (int32_t)(((int64_t)R[b] * (int64_t)R[c]) >> 32);
        DISPATCH();
    L_MULHU:
        R[a] = (int32_t)(((uint64_t)(uint32_t)R[b]
                        * (uint64_t)(uint32_t)R[c]) >> 32);
        DISPATCH();
    L_FADD:
        R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) + cvm_bits_to_f32(R[c]));
        DISPATCH();
    L_FSUB:
        R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) - cvm_bits_to_f32(R[c]));
        DISPATCH();
    L_FMUL:
        R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) * cvm_bits_to_f32(R[c]));
        DISPATCH();
    L_FDIV:
        /* IEEE 754 ÷0 = ±Inf (sign of dividend), 0/0 = NaN. No trap. */
        R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) / cvm_bits_to_f32(R[c]));
        DISPATCH();
    L_FNEG:
        /* Sign-bit toggle handles every value uniformly: -NaN stays NaN, -0 ↔ +0. */
        R[a] = R[b] ^ (int32_t)0x80000000;
        DISPATCH();
    L_FCMP_EQ:
        R[a] = (cvm_bits_to_f32(R[b]) == cvm_bits_to_f32(R[c])) ? 1 : 0;
        DISPATCH();
    L_FCMP_NE:
        R[a] = (cvm_bits_to_f32(R[b]) != cvm_bits_to_f32(R[c])) ? 1 : 0;
        DISPATCH();
    L_FCMP_LT:
        R[a] = (cvm_bits_to_f32(R[b]) <  cvm_bits_to_f32(R[c])) ? 1 : 0;
        DISPATCH();
    L_FCMP_LE:
        R[a] = (cvm_bits_to_f32(R[b]) <= cvm_bits_to_f32(R[c])) ? 1 : 0;
        DISPATCH();
    L_F2I_S:
        R[a] = cvm_f32_to_i32_sat(cvm_bits_to_f32(R[b]));
        DISPATCH();
    L_F2I_U:
        R[a] = (int32_t)cvm_f32_to_u32_sat(cvm_bits_to_f32(R[b]));
        DISPATCH();
    L_I2F_S:
        R[a] = cvm_f32_to_bits((float)R[b]);
        DISPATCH();
    L_I2F_U:
        R[a] = cvm_f32_to_bits((float)(uint32_t)R[b]);
        DISPATCH();
    L_JMPR: {
        uint32_t target = (uint32_t)R[a];
        if (target >= code_count) return CVM_E_BAD_PC;
        pc = target;
        DISPATCH();
    }
    L_FSQRT:
        R[a] = cvm_f32_to_bits(sqrtf(cvm_bits_to_f32(R[b])));
        DISPATCH();
    L_QDIV1616:
        if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
        R[a] = (int32_t)(uint32_t)((((uint64_t)(uint32_t)R[b]) << 16)
                                   / (uint32_t)R[c]);
        DISPATCH();
    L_QDIV6432:
        if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
        R[a] = (int32_t)(uint32_t)(((((uint64_t)(uint32_t)R[a]) << 32)
                                    | (uint32_t)R[b]) / (uint32_t)R[c]);
        DISPATCH();

    L_SETJMP: {
        uint32_t env = (uint32_t)R[b];
        if (env > mem_size || mem_size - env < 12u) return CVM_E_BAD_ADDR;
        uint32_t cur_sp = (uint32_t)R[CVM_REG_SP];
        uint32_t dest   = a;
        memcpy(heap + env + 0u, &pc, 4);       /* pc = the resume point */
        memcpy(heap + env + 4u, &cur_sp, 4);
        memcpy(heap + env + 8u, &dest, 4);
        R[a] = 0;
        DISPATCH();
    }
    L_LONGJMP: {
        uint32_t env = (uint32_t)R[a];
        if (env > mem_size || mem_size - env < 12u) return CVM_E_BAD_ADDR;
        uint32_t jpc, jsp, jdst;
        memcpy(&jpc,  heap + env + 0u, 4);
        memcpy(&jsp,  heap + env + 4u, 4);
        memcpy(&jdst, heap + env + 8u, 4);
        if (jpc >= code_count)        return CVM_E_BAD_PC;
        if (jdst >= CVM_REG_COUNT)    return CVM_E_BAD_ADDR;
        int32_t v = R[b];
        R[jdst]       = (v != 0) ? v : 1;      /* longjmp(env,0) appears as 1 */
        R[CVM_REG_SP] = (int32_t)jsp;
        pc            = jpc;
        DISPATCH();
    }

#  undef DISPATCH

#else  /* !CVM_THREADED — switch fallback for MSVC */

    for (;;) {
        if (pc >= code_count) return CVM_E_BAD_PC;
        PROF_TICK();
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
            if (addr > mem_size || mem_size - addr < 4u) return CVM_E_BAD_ADDR;
            int32_t v;
            memcpy(&v, heap + addr, 4);
            R[a] = v;
            break;
        }
        case CVM_OP_STW: {
            uint32_t addr = (uint32_t)R[b];
            if (addr > mem_size || mem_size - addr < 4u) return CVM_E_BAD_ADDR;
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
        case CVM_OP_CALL: {
            uint32_t fid = (inst >> 8) & 0xFFFFFFu;
            if (fid == 0) return CVM_E_NULL_FUNC_PTR;
            if (fid >= func_count) return CVM_E_BAD_FUNC_INDEX;
            uint32_t sp = (uint32_t)R[CVM_REG_SP];
            if (sp < 4u || sp > mem_size) return CVM_E_STACK_OVERFLOW;
            sp -= 4u;
            memcpy(heap + sp, &pc, 4);
            R[CVM_REG_SP] = (int32_t)sp;
            pc = func_offsets[fid];
            PROF_CALL(fid);
            break;
        }
        case CVM_OP_RET: {
            uint32_t sp = (uint32_t)R[CVM_REG_SP];
            if (sp > mem_size || mem_size - sp < 4u) return CVM_E_BAD_ADDR;
            uint32_t ret_pc;
            memcpy(&ret_pc, heap + sp, 4);
            R[CVM_REG_SP] = (int32_t)(sp + 4u);
            if (ret_pc == CVM_RET_SENTINEL) {
                if (return_value) *return_value = R[0];
                return CVM_OK;
            }
            pc = ret_pc;
            PROF_RET();
            break;
        }
        case CVM_OP_CALLR: {
            uint32_t fid = (uint32_t)R[a];
            if (fid == 0) return CVM_E_NULL_FUNC_PTR;
            if (fid >= func_count) return CVM_E_BAD_FUNC_INDEX;
            uint32_t sp = (uint32_t)R[CVM_REG_SP];
            if (sp < 4u || sp > mem_size) return CVM_E_STACK_OVERFLOW;
            sp -= 4u;
            memcpy(heap + sp, &pc, 4);
            R[CVM_REG_SP] = (int32_t)sp;
            pc = func_offsets[fid];
            PROF_CALL(fid);
            break;
        }
        case CVM_OP_LDB: {
            uint32_t addr = (uint32_t)R[b];
            if (addr >= mem_size) return CVM_E_BAD_ADDR;
            R[a] = (int32_t)(uint32_t)heap[addr];
            break;
        }
        case CVM_OP_STB: {
            uint32_t addr = (uint32_t)R[b];
            if (addr >= mem_size) return CVM_E_BAD_ADDR;
            heap[addr] = (uint8_t)((uint32_t)R[c] & 0xFFu);
            break;
        }
        case CVM_OP_LDH: {
            uint32_t addr = (uint32_t)R[b];
            if (addr > mem_size || mem_size - addr < 2u) return CVM_E_BAD_ADDR;
            uint16_t v;
            memcpy(&v, heap + addr, 2);
            R[a] = (int32_t)(uint32_t)v;
            break;
        }
        case CVM_OP_STH: {
            uint32_t addr = (uint32_t)R[b];
            if (addr > mem_size || mem_size - addr < 2u) return CVM_E_BAD_ADDR;
            uint16_t v = (uint16_t)((uint32_t)R[c] & 0xFFFFu);
            memcpy(heap + addr, &v, 2);
            break;
        }
        case CVM_OP_MOVHI: {
            uint32_t hi = (inst >> 16) & 0xFFFFu;
            R[a] = (int32_t)((hi << 16) | ((uint32_t)R[a] & 0xFFFFu));
            break;
        }
        case CVM_OP_MEMCPY: {
            uint32_t dst = (uint32_t)R[a];
            uint32_t src = (uint32_t)R[b];
            uint32_t len = (uint32_t)R[c];
            if (len) {
                if (dst > mem_size || mem_size - dst < len) return CVM_E_BAD_ADDR;
                if (src > mem_size || mem_size - src < len) return CVM_E_BAD_ADDR;
                memcpy(heap + dst, heap + src, len);
            }
            break;
        }
        case CVM_OP_MEMSET: {
            uint32_t dst = (uint32_t)R[a];
            uint32_t len = (uint32_t)R[c];
            if (len) {
                if (dst > mem_size || mem_size - dst < len) return CVM_E_BAD_ADDR;
                memset(heap + dst, (int)((uint32_t)R[b] & 0xFFu), len);
            }
            break;
        }
        case CVM_OP_MEMMOVE: {
            uint32_t dst = (uint32_t)R[a];
            uint32_t src = (uint32_t)R[b];
            uint32_t len = (uint32_t)R[c];
            if (len) {
                if (dst > mem_size || mem_size - dst < len) return CVM_E_BAD_ADDR;
                if (src > mem_size || mem_size - src < len) return CVM_E_BAD_ADDR;
                memmove(heap + dst, heap + src, len);
            }
            break;
        }
        case CVM_OP_MULH:
            R[a] = (int32_t)(((int64_t)R[b] * (int64_t)R[c]) >> 32);
            break;
        case CVM_OP_MULHU:
            R[a] = (int32_t)(((uint64_t)(uint32_t)R[b]
                            * (uint64_t)(uint32_t)R[c]) >> 32);
            break;
        case CVM_OP_FADD:
            R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) + cvm_bits_to_f32(R[c]));
            break;
        case CVM_OP_FSUB:
            R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) - cvm_bits_to_f32(R[c]));
            break;
        case CVM_OP_FMUL:
            R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) * cvm_bits_to_f32(R[c]));
            break;
        case CVM_OP_FDIV:
            R[a] = cvm_f32_to_bits(cvm_bits_to_f32(R[b]) / cvm_bits_to_f32(R[c]));
            break;
        case CVM_OP_FNEG:
            R[a] = R[b] ^ (int32_t)0x80000000;
            break;
        case CVM_OP_FCMP_EQ:
            R[a] = (cvm_bits_to_f32(R[b]) == cvm_bits_to_f32(R[c])) ? 1 : 0;
            break;
        case CVM_OP_FCMP_NE:
            R[a] = (cvm_bits_to_f32(R[b]) != cvm_bits_to_f32(R[c])) ? 1 : 0;
            break;
        case CVM_OP_FCMP_LT:
            R[a] = (cvm_bits_to_f32(R[b]) <  cvm_bits_to_f32(R[c])) ? 1 : 0;
            break;
        case CVM_OP_FCMP_LE:
            R[a] = (cvm_bits_to_f32(R[b]) <= cvm_bits_to_f32(R[c])) ? 1 : 0;
            break;
        case CVM_OP_F2I_S:
            R[a] = cvm_f32_to_i32_sat(cvm_bits_to_f32(R[b]));
            break;
        case CVM_OP_F2I_U:
            R[a] = (int32_t)cvm_f32_to_u32_sat(cvm_bits_to_f32(R[b]));
            break;
        case CVM_OP_I2F_S:
            R[a] = cvm_f32_to_bits((float)R[b]);
            break;
        case CVM_OP_I2F_U:
            R[a] = cvm_f32_to_bits((float)(uint32_t)R[b]);
            break;
        case CVM_OP_JMPR: {
            uint32_t target = (uint32_t)R[a];
            if (target >= code_count) return CVM_E_BAD_PC;
            pc = target;
            break;
        }
        case CVM_OP_FSQRT:
            R[a] = cvm_f32_to_bits(sqrtf(cvm_bits_to_f32(R[b])));
            break;
        case CVM_OP_QDIV1616:
            if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
            R[a] = (int32_t)(uint32_t)((((uint64_t)(uint32_t)R[b]) << 16)
                                       / (uint32_t)R[c]);
            break;
        case CVM_OP_QDIV6432:
            if (R[c] == 0) return CVM_E_DIV_BY_ZERO;
            R[a] = (int32_t)(uint32_t)(((((uint64_t)(uint32_t)R[a]) << 32)
                                        | (uint32_t)R[b]) / (uint32_t)R[c]);
            break;
        case CVM_OP_SETJMP: {
            uint32_t env = (uint32_t)R[b];
            if (env > mem_size || mem_size - env < 12u) return CVM_E_BAD_ADDR;
            uint32_t cur_sp = (uint32_t)R[CVM_REG_SP];
            uint32_t dest   = a;
            memcpy(heap + env + 0u, &pc, 4);
            memcpy(heap + env + 4u, &cur_sp, 4);
            memcpy(heap + env + 8u, &dest, 4);
            R[a] = 0;
            break;
        }
        case CVM_OP_LONGJMP: {
            uint32_t env = (uint32_t)R[a];
            if (env > mem_size || mem_size - env < 12u) return CVM_E_BAD_ADDR;
            uint32_t jpc, jsp, jdst;
            memcpy(&jpc,  heap + env + 0u, 4);
            memcpy(&jsp,  heap + env + 4u, 4);
            memcpy(&jdst, heap + env + 8u, 4);
            if (jpc >= code_count)     return CVM_E_BAD_PC;
            if (jdst >= CVM_REG_COUNT) return CVM_E_BAD_ADDR;
            int32_t v = R[b];
            R[jdst]       = (v != 0) ? v : 1;
            R[CVM_REG_SP] = (int32_t)jsp;
            pc            = jpc;
            break;
        }
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
