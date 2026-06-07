#include "cvm.h"

#include <math.h>     /* sqrtf()/floorf()/ceilf()/truncf() — used by CVM_OP_FSQRT
                         and the FFLOOR/FCEIL/FTRUNC family. On hosts without
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
/* Debug instruction cap: when >0, execution returns cleanly once cvm_prof_total
 * reaches it. Lets a profiling run break out of a runaway/infinite loop and dump
 * the dominant (spinning) function. 0 = no cap. Set by the host before running. */
uint64_t  cvm_prof_cap    = 0;

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
/* Charge this instruction to the function whose [entry,next) range holds pc. The
 * range is cached in [prof_lo,prof_hi); only re-bsearch on a boundary crossing. */
#  define PROF_TICK()    do { \
        if (pc < prof_lo || pc >= prof_hi) { \
            /* User function symidx s lives at FUNCS slot s<<1 (odd slots are dup/aux); \
             * binary-search the symidx whose entry func_offsets[s<<1] is the greatest \
             * <= pc, so prof_cur == the .sym index (= cvm-translate's k+1). */ \
            uint32_t _ns = func_count >> 1, _lo = 1, _hi = _ns, _ans = 0; \
            while (_lo < _hi) { uint32_t _m = (_lo + _hi) >> 1; \
                if (func_offsets[_m << 1] <= pc) { _ans = _m; _lo = _m + 1; } else _hi = _m; } \
            prof_cur = _ans; \
            prof_lo  = _ans ? func_offsets[_ans << 1] : 0u; \
            prof_hi  = (_ans + 1u < _ns) ? func_offsets[(_ans + 1u) << 1] : code_count; \
        } \
        if (prof_cur < cvm_prof_len) cvm_prof_counts[prof_cur]++; \
        cvm_prof_total++; \
        if (cvm_prof_cap && cvm_prof_total >= cvm_prof_cap) return CVM_OK; \
    } while (0)
/* Caller-attribution for the watched fid: at the call, pc is still in the caller,
 * so the pc-derived prof_cur already names the caller. No shadow stack needed. */
#  define PROF_CALL(fid) do { if ((uint32_t)(fid) == cvm_prof_watch && prof_cur < cvm_prof_len) \
                                  cvm_prof_caller[prof_cur]++; } while (0)
#  define PROF_RET()        ((void)0)
#  define PROF_LONGJMP(jsp) ((void)0)
#else
#  define PROF_TICK()    ((void)0)
#  define PROF_CALL(fid) ((void)0)
#  define PROF_RET()     ((void)0)
#  define PROF_LONGJMP(jsp) ((void)0)
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
    case CVM_E_BAD_CORO_STATE:   return "coro swap to a RUNNING or DEAD coroutine";
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

/* ===========================================================================
 *  VM diagnostics subsystem  (compile flag: CVM_DIAG; CMake: -DCVM_DIAG=ON)
 * ---------------------------------------------------------------------------
 *  An opt-in, zero-overhead-when-off set of instruments for chasing memory
 *  corruption and codegen miscompiles in carts running on the VM. Every probe
 *  is gated at COMPILE time by CVM_DIAG (so production builds carry none of it)
 *  and selected at RUN time by an environment variable (so one debug build
 *  serves every probe). All output goes to stderr, prefixed by the probe name.
 *
 *  This toolkit cracked the Exult `Usecode_internal::run()` spilled-alloca
 *  corruption (a 1.4M-element relocate over-walking the heap from a garbage
 *  near-NULL vector `this`); docs/debugging.md has the full worked example.
 *
 *  Probes (see docs/debugging.md for the complete reference):
 *    CVM_REDZONE=1        Bounds-checking allocator shadow: hook the cart's
 *                         malloc/free/realloc/calloc (FUNCS slots via
 *                         CVM_RZ_MALLOC/FREE/REALLOC/CALLOC=<slot>) and TRAP on
 *                         the first store landing outside any live allocation,
 *                         with a heuristic backtrace, holder scan, register
 *                         dump, a stack-frame window, and the ring/trip
 *                         captures (CVM_RING_A/B, CVM_TRIP_PC) below.
 *    CVM_PCDUMP=<pc>      Dump R[a/b/c]+SP+args + 16 bytes at R[b] when pc is
 *                         reached; filters CVM_PCDUMP_RB / _R1 / _SP=<val>.
 *    CVM_WADDR=<addr>     Log every store overlapping addr (with pc+func).
 *    CVM_WVAL=<val>       Log every 4-byte store of val (with addr+pc+func).
 *    CVM_MISP=<1|2>       Flag a store/memcpy writing a MISALIGNED in-heap ptr
 *                         into a 4-aligned heap slot; CVM_MISP_TARGET=<val>
 *                         watches one value over stw+memcpy; _FUNC / _AND3 filter.
 *    CVM_RING_A/B=<addr>  Ring buffer of the last RING_N writes (pc, old->new)
 *                         to a watched slot; dumped at the redzone trap (used to
 *                         reconstruct a loop cursor's full history without
 *                         drowning in millions of global writes).
 *    CVM_TRIP_PC=<pc>     Capture R[b] at pc (CVM_TRIP_SP=<sp> filters by frame);
 *                         reported at the redzone trap with the field's last
 *                         writer (pins who wrote a corrupt pointer to an object).
 *  A per-word last-writer-PC map over [heap_start, mem_size) backs the holder
 *  scan and the trip report (active whenever CVM_REDZONE is set).
 * =========================================================================== */
#if defined(CVM_DIAG)
#include <stdlib.h>
#include <stdio.h>
static long     g_dg_pc   = -2;
static uint32_t g_dg_wa   = 0;
static int      g_dg_won  = -1;
static long     g_dg_wval = -2;   /* CVM_WVAL: log any store of this 32-bit value */
static long     g_dg_rb   = -2;   /* CVM_PCDUMP_RB: only dump when R[b]==this */
static long     g_dg_r1   = -2;   /* CVM_PCDUMP_R1: only dump when R[1]==this */
static long     g_dg_sp   = -2;   /* CVM_PCDUMP_SP: only dump when SP==this */
/* CVM_MISP: catch the first store that puts a MISALIGNED (&3!=0) in-heap pointer
 * into a 4-aligned heap slot. mode 1 = only when the slot previously held an
 * ALIGNED in-heap pointer (a __begin_-style field going bad); mode 2 = any. */
static int      g_misp   = -2;
static int      g_misp_n = 0;
static long     g_misp_target = -1;   /* CVM_MISP_TARGET: watch this exact value (stw+memcpy) */
static long     g_misp_func   = -1;   /* CVM_MISP_FUNC: only log this dg_enc func */
static int      g_misp_and3   = -1;   /* CVM_MISP_AND3: only log when new&3 == this */
static uint32_t g_hs = 0, g_he = 0;   /* heap [start,end) for misp (CVM_REDZONE-independent) */
/* Ring buffers: the last RING_N writes (pc, old->new) to up to two watched
 * stack slots, dumped at the redzone trap — reconstructs a cursor slot's full
 * write history (init + every +20 advance + the wild write) without drowning
 * in ~700k global writes. CVM_RING_A / CVM_RING_B = the slot addresses. */
#define RING_N 256u   /* power of two */
static uint32_t ring_a = 0xffffffffu, ring_b = 0xffffffffu;
static uint32_t ring_a_pc[RING_N], ring_a_old[RING_N], ring_a_new[RING_N];
static uint32_t ring_b_pc[RING_N], ring_b_old[RING_N], ring_b_new[RING_N];
static uint32_t ring_a_i = 0, ring_b_i = 0;
/* Trip-wire: at CVM_TRIP_PC capture R[b] (e.g. the vector `this` in a LDW
 * [Rb]); reported at the redzone trap with its first heap word + that field's
 * last writer — i.e. who wrote the corrupt misaligned __begin_. */
static uint32_t trip_pc = 0xffffffffu, trip_rb = 0, trip_sp = 0;
static void dg_init(void) {
    if (g_misp == -2) { const char *e = getenv("CVM_MISP"); g_misp = e ? atoi(e) : 0;
                        const char *t = getenv("CVM_MISP_TARGET");
                        if (t) { g_misp_target = (long)(uint32_t)strtoul(t, 0, 0); if (g_misp <= 0) g_misp = 3; }
                        const char *f = getenv("CVM_MISP_FUNC"); if (f) g_misp_func = strtol(f, 0, 0);
                        const char *a = getenv("CVM_MISP_AND3"); if (a) g_misp_and3 = atoi(a); }
    if (g_dg_pc == -2) { const char *e = getenv("CVM_PCDUMP"); g_dg_pc = e ? strtol(e, 0, 0) : -1; }
    if (g_dg_won < 0)  { const char *e = getenv("CVM_WADDR");
                         if (e) { g_dg_wa = (uint32_t)strtoul(e, 0, 0); g_dg_won = 1; } else g_dg_won = 0; }
    if (g_dg_wval == -2) { const char *e = getenv("CVM_WVAL");
                           g_dg_wval = e ? (long)(uint32_t)strtoul(e, 0, 0) : -1; }
    if (g_dg_rb == -2) { const char *e = getenv("CVM_PCDUMP_RB");
                         g_dg_rb = e ? (long)(uint32_t)strtoul(e, 0, 0) : -1; }
    if (g_dg_r1 == -2) { const char *e = getenv("CVM_PCDUMP_R1");
                         g_dg_r1 = e ? (long)(uint32_t)strtoul(e, 0, 0) : -1; }
    if (g_dg_sp == -2) { const char *e = getenv("CVM_PCDUMP_SP");
                         g_dg_sp = e ? (long)(uint32_t)strtoul(e, 0, 0) : -1; }
    { const char *e = getenv("CVM_RING_A"); if (e) ring_a = (uint32_t)strtoul(e, 0, 0); }
    { const char *e = getenv("CVM_RING_B"); if (e) ring_b = (uint32_t)strtoul(e, 0, 0); }
    { const char *e = getenv("CVM_TRIP_PC"); if (e) trip_pc = (uint32_t)strtoul(e, 0, 0); }
    { const char *e = getenv("CVM_TRIP_SP"); if (e) trip_sp = (uint32_t)strtoul(e, 0, 0); }
}
static uint32_t dg_enc(uint32_t pc, const uint32_t *fo, uint32_t fc) {
    uint32_t best = 0, e = 0;
    for (uint32_t i = 1; i < fc; i++) if (fo[i] <= pc && fo[i] >= best) { best = fo[i]; e = i; }
    return e;
}
/* Per-stack-word last-writer-PC map (CVM_DIAG): records the pc of the last
 * store to each 4-byte stack slot so the redzone trap can NAME who wrote a
 * wild relocate cursor. Covers [lw_base, lw_base + lw_words*4) = the stack. */
static uint32_t *lw_pc   = NULL;
static uint32_t  lw_base = 0, lw_words = 0;
static void lw_rec(uint32_t pc, uint32_t addr, uint32_t sz) {
    if (!lw_pc) return;
    uint32_t w = addr & ~3u, end = addr + sz;
    for (; w < end; w += 4u) {
        if (w < lw_base) continue;
        uint32_t i = (w - lw_base) >> 2;
        if (i < lw_words) lw_pc[i] = pc;
    }
}
static uint32_t lw_get(uint32_t addr) {
    if (!lw_pc || (addr & 3u) || addr < lw_base) return 0;
    uint32_t i = (addr - lw_base) >> 2;
    return i < lw_words ? lw_pc[i] : 0;
}
/* Record one 4-byte store to a watched ring slot (old read from heap = value
 * BEFORE the store, since DG_W runs before the heap memcpy). */
static void ring_rec(uint32_t pc, uint32_t addr, uint32_t sz, uint32_t nv,
                     const uint8_t *heap) {
    if (sz != 4u) return;
    if (addr == ring_a) { uint32_t ov; memcpy(&ov, heap + addr, 4);
        uint32_t i = ring_a_i++ & (RING_N - 1u);
        ring_a_pc[i] = pc; ring_a_old[i] = ov; ring_a_new[i] = nv; }
    if (addr == ring_b) { uint32_t ov; memcpy(&ov, heap + addr, 4);
        uint32_t i = ring_b_i++ & (RING_N - 1u);
        ring_b_pc[i] = pc; ring_b_old[i] = ov; ring_b_new[i] = nv; }
}
static void ring_dump(const char *nm, uint32_t addr, const uint32_t *rpc,
                      const uint32_t *rold, const uint32_t *rnew, uint32_t cnt,
                      const uint32_t *fo, uint32_t fc) {
    if (addr == 0xffffffffu) return;
    uint32_t n = cnt < RING_N ? cnt : RING_N;
    fprintf(stderr, "[RING-%s] @0x%08x last %u writes (oldest->newest):\n", nm, addr, n);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (cnt - n + k) & (RING_N - 1u);
        fprintf(stderr, "    pc=%u f=%u  0x%08x -> 0x%08x\n",
                rpc[i], dg_enc(rpc[i], fo, fc), rold[i], rnew[i]);
    }
    fflush(stderr);
}
static void dg_pcdump(uint32_t pc, uint8_t op, uint8_t a, uint8_t b, uint8_t c,
                      const int32_t *R, uint32_t sp_reg,
                      const uint8_t *heap, uint32_t mem_size) {
    fprintf(stderr, "[PCDUMP] pc=%u op=%u a=%u R[a]=0x%08x b=%u R[b]=0x%08x "
            "c=%u R[c]=0x%08x SP=0x%08x\n", pc, op, a, (uint32_t)R[a], b,
            (uint32_t)R[b], c, (uint32_t)R[c], sp_reg);
    /* Dump 16 bytes at R[b] (e.g. a std::string object: cap/size/data words). */
    uint32_t s = (uint32_t)R[b];
    if (s + 16u <= mem_size) {
        fprintf(stderr, "       [R[b]] words: %08x %08x %08x %08x  bytes:",
                ((const uint32_t *)(heap + s))[0], ((const uint32_t *)(heap + s))[1],
                ((const uint32_t *)(heap + s))[2], ((const uint32_t *)(heap + s))[3]);
        for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", heap[s + i]);
        fprintf(stderr, "\n");
    }
    /* args R0/R1 + the word at SP (= return PC at a function entry) + R8..R40. */
    uint32_t spv = (uint32_t)R[CVM_REG_SP];
    uint32_t retpc = (spv + 4u <= mem_size) ? *((const uint32_t *)(heap + spv)) : 0;
    fprintf(stderr, "       R0=0x%08x R1=0x%08x [SP]=ret_pc=%u\n",
            (uint32_t)R[0], (uint32_t)R[1], retpc);
    fprintf(stderr, "       regs:");
    for (int i = 8; i <= 40; i++) fprintf(stderr, " R%d=0x%08x", i, (uint32_t)R[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}
static void dg_wlog(uint32_t pc, uint32_t addr, uint32_t sz, uint32_t val,
                    const uint32_t *fo, uint32_t fc) {
    lw_rec(pc, addr, sz);
    if (g_dg_won && addr < g_dg_wa + 4u && addr + sz > g_dg_wa)
        fprintf(stderr, "[WADDR] store @0x%08x sz=%u val=0x%08x pc=%u func=%u\n",
                addr, sz, val, pc, dg_enc(pc, fo, fc));
    if (g_dg_wval >= 0 && val == (uint32_t)g_dg_wval && sz == 4u)
        fprintf(stderr, "[WVAL] val=0x%08x stored @0x%08x pc=%u func=%u\n",
                val, addr, pc, dg_enc(pc, fo, fc));
}
/* One 4-aligned heap word transition old->new (kind: 0=stw scalar, 1=memcpy). */
static void misp_word(uint32_t pc, uint32_t addr, uint32_t oldv, uint32_t newv,
                      int kind, const uint32_t *fo, uint32_t fc) {
    if (g_misp <= 0 || g_misp_n >= 400) return;
    uint32_t f = 0;
    if (g_misp_target >= 0) {                       /* target mode: exact value, any kind */
        if (newv != (uint32_t)g_misp_target) return;
    } else {
        if ((addr & 3u) || addr < g_hs || addr >= g_he) return;        /* aligned heap slot */
        if (!(newv >= g_hs && newv < g_he && (newv & 3u))) return;      /* new = misaligned in-heap ptr */
        if (g_misp == 1 && !(oldv >= g_hs && oldv < g_he && (oldv & 3u) == 0))
            return;                                                    /* mode 1: old = aligned in-heap ptr */
        if (g_misp_and3 >= 0 && (int)(newv & 3u) != g_misp_and3) return;
    }
    f = dg_enc(pc, fo, fc);
    if (g_misp_func >= 0 && (long)f != g_misp_func) return;
    fprintf(stderr, "[MISP#%d] @0x%08x old=0x%08x new=0x%08x pc=%u func=%u kind=%s\n",
            g_misp_n++, addr, oldv, newv, pc, f, kind ? "cpy" : "stw");
    fflush(stderr);
}
/* Scan a pending MEMCPY/MEMMOVE for misaligned-pointer words about to land in
 * aligned heap slots (the begin-propagation path). Call BEFORE the copy. */
static void misp_scan(uint32_t pc, uint32_t dst, uint32_t src, uint32_t len,
                      const uint8_t *heap, uint32_t mem, const uint32_t *fo, uint32_t fc) {
    if (g_misp <= 0) return;
    for (uint32_t i = 0; i + 4u <= len; i += 4u) {
        uint32_t d = dst + i; if (d & 3u) continue;
        if (src + i + 4u > mem || d + 4u > mem) continue;
        uint32_t nv, ov; memcpy(&nv, heap + src + i, 4); memcpy(&ov, heap + d, 4);
        misp_word(pc, d, ov, nv, 1, fo, fc);
    }
}
/* ---- redzone / bounds-checking allocator (CVM_REDZONE) -------------------
 * Hook the cart's malloc/free/realloc/calloc (FUNCS slots via env), keep a
 * per-heap-byte shadow (1 = live user allocation, 0 = redzone/free/metadata),
 * skip checks while inside the allocator, and TRAP on the first cart store
 * landing on a 0 byte → the original out-of-bounds heap write. */
static int      rz_on    = -1;
static uint8_t *rz_shadow = NULL;
static uint32_t rz_hs = 0, rz_he = 0;             /* heap [start,end) */
static uint32_t rz_malloc, rz_free, rz_realloc, rz_calloc;   /* FUNCS slots */
#define RZ_MAPN 0x100000u                          /* ptr->size open-addr map */
static uint32_t *rz_key, *rz_val;
static int       rz_depth = 0, rz_ad = -1, rz_kind = 0;      /* call depth / in-alloc depth / kind */
static uint32_t  rz_a0 = 0, rz_a1 = 0;
static void rz_minit(void) {
    rz_key = (uint32_t *)calloc(RZ_MAPN, 4);
    rz_val = (uint32_t *)calloc(RZ_MAPN, 4);
}
static void rz_put(uint32_t p, uint32_t s) {
    if (!p) return;
    uint32_t h = (p >> 3) & (RZ_MAPN - 1);
    for (uint32_t i = 0; i < RZ_MAPN; i++) {
        uint32_t j = (h + i) & (RZ_MAPN - 1);
        if (rz_key[j] == 0 || rz_key[j] == p) { rz_key[j] = p; rz_val[j] = s; return; }
    }
}
static uint32_t rz_get(uint32_t p) {
    uint32_t h = (p >> 3) & (RZ_MAPN - 1);
    for (uint32_t i = 0; i < RZ_MAPN; i++) {
        uint32_t j = (h + i) & (RZ_MAPN - 1);
        if (rz_key[j] == 0) return 0;
        if (rz_key[j] == p) return rz_val[j];
    }
    return 0;
}
static void rz_del(uint32_t p) {
    uint32_t h = (p >> 3) & (RZ_MAPN - 1);
    for (uint32_t i = 0; i < RZ_MAPN; i++) {
        uint32_t j = (h + i) & (RZ_MAPN - 1);
        if (rz_key[j] == 0) return;
        if (rz_key[j] == p) { rz_key[j] = 0; rz_val[j] = 0; return; }
    }
}
static void rz_mark(uint32_t a, uint32_t s, uint8_t v) {
    if (!rz_shadow || a < rz_hs) return;
    uint32_t end = a + s; if (end > rz_he) end = rz_he; if (a >= end) return;
    memset(rz_shadow + (a - rz_hs), v, end - a);
}
static void rz_setup(uint32_t hs, uint32_t he, const uint32_t *fo, uint32_t fc) {
    if (rz_on >= 0) return;
    const char *e = getenv("CVM_REDZONE");
    if (!e) { rz_on = 0; return; }
    rz_on = 1; rz_hs = hs; rz_he = he;
    rz_shadow = (uint8_t *)calloc(he - hs, 1);
    rz_minit();
    /* slots from env (idx*2); 0 = unhooked. */
    const char *m = getenv("CVM_RZ_MALLOC"), *f = getenv("CVM_RZ_FREE");
    const char *r = getenv("CVM_RZ_REALLOC"), *c = getenv("CVM_RZ_CALLOC");
    rz_malloc  = m ? (uint32_t)strtoul(m, 0, 0) : 0;
    rz_free    = f ? (uint32_t)strtoul(f, 0, 0) : 0;
    rz_realloc = r ? (uint32_t)strtoul(r, 0, 0) : 0;
    rz_calloc  = c ? (uint32_t)strtoul(c, 0, 0) : 0;
    fprintf(stderr, "[RZ] on heap=[0x%08x,0x%08x) malloc=%u free=%u realloc=%u calloc=%u\n",
            hs, he, rz_malloc, rz_free, rz_realloc, rz_calloc);
    fflush(stderr); (void)fo; (void)fc;
}
static void rz_call(uint32_t fid, const int32_t *R) {
    if (rz_on != 1) return;
    rz_depth++;
    if (rz_ad >= 0) return;                         /* already inside an allocator */
    int k = 0;
    if      (fid == rz_malloc)  k = 1;
    else if (fid == rz_calloc)  k = 2;
    else if (fid == rz_realloc) k = 3;
    else if (fid == rz_free)    k = 4;
    if (!k) return;
    rz_ad = rz_depth; rz_kind = k;
    rz_a0 = (uint32_t)R[0]; rz_a1 = (uint32_t)R[1];
}
static void rz_ret(const int32_t *R, uint32_t pc, const uint32_t *fo, uint32_t fc) {
    if (rz_on != 1) { return; }
    if (rz_ad == rz_depth) {                        /* returning from the allocator */
        uint32_t res = (uint32_t)R[0];
        if (rz_kind == 1) { if (res) { rz_mark(res, rz_a0, 1); rz_put(res, rz_a0); } }
        else if (rz_kind == 2) { uint32_t s = rz_a0 * rz_a1; if (res) { rz_mark(res, s, 1); rz_put(res, s); } }
        else if (rz_kind == 4) { if (rz_a0) { rz_mark(rz_a0, rz_get(rz_a0), 0); rz_del(rz_a0); } }
        else if (rz_kind == 3) {                    /* realloc(old=a0,newsize=a1)->res */
            uint32_t os = rz_get(rz_a0);
            if (res && res != rz_a0) {
                if (rz_a0) { rz_mark(rz_a0, os, 0); rz_del(rz_a0); }
                rz_mark(res, rz_a1, 1); rz_put(res, rz_a1);
            } else if (res) {
                if (rz_a1 > os) rz_mark(rz_a0 + os, rz_a1 - os, 1);
                else            rz_mark(rz_a0 + rz_a1, os - rz_a1, 0);
                rz_put(res, rz_a1);
            }
        }
        rz_ad = -1; rz_kind = 0;
    }
    if (rz_depth > 0) rz_depth--;
    (void)pc; (void)fo; (void)fc;
}
static void rz_check(uint32_t addr, uint32_t sz, uint32_t pc, const int32_t *R,
                     const uint32_t *fo, uint32_t fc,
                     const uint8_t *heap, uint32_t mem_size) {
    if (rz_on != 1 || rz_ad >= 0 || !rz_shadow) return;
    if (addr < rz_hs || addr >= rz_he) return;
    uint32_t end = addr + sz; if (end > rz_he) end = rz_he;
    for (uint32_t p = addr; p < end; p++)
        if (rz_shadow[p - rz_hs] == 0) {
            fprintf(stderr, "[RZ-TRAP] OOB heap store @0x%08x sz=%u pc=%u func=%u "
                    "R[c]=0x%08x (first bad byte 0x%08x)\n",
                    addr, sz, pc, dg_enc(pc, fo, fc), (uint32_t)R[2], p);
            /* Heuristic backtrace: scan the stack from SP for words that are
             * valid code PCs sitting just after a CALL (return addresses). */
            uint32_t sp = (uint32_t)R[CVM_REG_SP];
            fprintf(stderr, "[RZ-BT] func chain (outer->):");
            int shown = 0;
            for (uint32_t s = sp; s + 4u <= mem_size && shown < 24; s += 4) {
                uint32_t w; memcpy(&w, heap + s, 4);
                if (w > 0 && w < mem_size && w < 16000000u) {  /* plausible code pc */
                    uint32_t f = dg_enc(w, fo, fc);
                    if (f) { fprintf(stderr, " %u@pc%u", f, w); shown++; }
                }
            }
            fprintf(stderr, "\n");
            /* Registers at the trap: capture dest/source/begin cursors. */
            fprintf(stderr, "[RZ-REGS]");
            for (int i = 0; i <= 40; i++) fprintf(stderr, " R%d=0x%08x", i, (uint32_t)R[i]);
            fprintf(stderr, " R255(SP)=0x%08x\n", sp);
            /* Scan all memory for words == the OOB address / source cursor and
             * report each holder slot + the pc/func that LAST WROTE it (= who
             * wrote the wild relocate cursor). */
            uint32_t want2 = addr - 4u;   /* the misaligned source object (other) */
            fprintf(stderr, "[RZ-HOLDERS] words == 0x%08x (oob addr) at:", addr);
            int hn = 0;
            for (uint32_t s = 0; s + 4u <= mem_size && hn < 40; s += 4) {
                uint32_t w; memcpy(&w, heap + s, 4);
                if (w == addr) { uint32_t lw = lw_get(s);
                    fprintf(stderr, " 0x%08x(W %u@f%u)", s, lw, lw ? dg_enc(lw, fo, fc) : 0); hn++; }
            }
            fprintf(stderr, "\n[RZ-HOLDERS2] words == 0x%08x (source cursor) at:", want2);
            hn = 0;
            for (uint32_t s = 0; s + 4u <= mem_size && hn < 40; s += 4) {
                uint32_t w; memcpy(&w, heap + s, 4);
                if (w == want2) { uint32_t lw = lw_get(s);
                    fprintf(stderr, " 0x%08x(W %u@f%u)", s, lw, lw ? dg_enc(lw, fo, fc) : 0); hn++; }
            }
            fprintf(stderr, "\n");
            /* Frame window from SP upward: annotate code-PCs (return addresses),
             * in-heap (mis)aligned pointers, and each slot's last writer. This
             * exposes the nested emplace frame + its cursor slots in one shot. */
            fprintf(stderr, "[RZ-FRAME] window from sp=0x%08x:\n", sp);
            for (uint32_t i = 0; i < 1024u; i++) {
                uint32_t s = sp + i * 4u; if (s + 4u > mem_size) break;
                uint32_t w; memcpy(&w, heap + s, 4);
                const char *tag = NULL; char tb[48];
                if (w > 0 && w < 16000000u) { uint32_t f = dg_enc(w, fo, fc);
                    if (f) { snprintf(tb, sizeof tb, "ret %u@pc%u", f, w); tag = tb; } }
                if (!tag && w >= rz_hs && w < rz_he) {
                    snprintf(tb, sizeof tb, "heapptr%s", (w & 3u) ? " *MISALIGNED*" : ""); tag = tb; }
                uint32_t lw = lw_get(s);
                if (tag || lw)
                    fprintf(stderr, "  [0x%08x]=0x%08x  %-22s lastW=%u@f%u\n",
                            s, w, tag ? tag : "", lw, lw ? dg_enc(lw, fo, fc) : 0);
            }
            ring_dump("A", ring_a, ring_a_pc, ring_a_old, ring_a_new, ring_a_i, fo, fc);
            ring_dump("B", ring_b, ring_b_pc, ring_b_old, ring_b_new, ring_b_i, fo, fc);
            if (trip_rb) {     /* trip captured the vector `this` (R8 at CVM_TRIP_PC) */
                uint32_t bf = 0; if (trip_rb + 4u <= mem_size) memcpy(&bf, heap + trip_rb, 4);
                uint32_t end = 0; if (trip_rb + 8u <= mem_size) memcpy(&end, heap + trip_rb + 4u, 4);
                uint32_t cap = 0; if (trip_rb + 12u <= mem_size) memcpy(&cap, heap + trip_rb + 8u, 4);
                uint32_t w = lw_get(trip_rb);
                fprintf(stderr, "[RZ-TRIP] vec this=0x%08x  __begin_=0x%08x%s __end_=0x%08x __cap_=0x%08x"
                        "  beginFieldLastW=%u@f%u\n", trip_rb, bf,
                        (bf >= rz_hs && bf < rz_he && (bf & 3u)) ? " *MISALIGNED*" : "",
                        end, cap, w, w ? dg_enc(w, fo, fc) : 0);
            }
            fflush(stderr);
            rz_on = 2;                              /* one-shot: report once */
            return;
        }
}
#  define RZ_CALL(fid)        do { if (rz_on == 1) rz_call((fid), R); } while (0)
#  define RZ_RET()            do { if (rz_on == 1) rz_ret(R, pc, func_offsets, func_count); } while (0)
#  define RZ_CHECK(addr, sz)  do { if (rz_on == 1) rz_check((addr), (sz), (pc) - 1u, R, func_offsets, func_count, heap, mem_size); } while (0)
#  define DG_W(addr, sz, val) do { dg_wlog((pc) - 1u, (addr), (sz), (uint32_t)(val), func_offsets, func_count); \
                                    ring_rec((pc) - 1u, (addr), (sz), (uint32_t)(val), heap); \
                                    RZ_CHECK((addr), (sz)); \
                                    if (g_misp > 0 && (sz) == 4u) { uint32_t _mo; memcpy(&_mo, heap + (addr), 4); \
                                        misp_word((pc) - 1u, (addr), _mo, (uint32_t)(val), 0, func_offsets, func_count); } } while (0)
#  define MISP_SCAN(dst, src, len) do { if (g_misp > 0) misp_scan((pc) - 1u, (dst), (src), (len), heap, mem_size, func_offsets, func_count); } while (0)
#  define DG_HOOK() do { if (g_dg_pc >= 0 && (long)((pc) - 1u) == g_dg_pc \
                            && (g_dg_rb < 0 || (uint32_t)R[b] == (uint32_t)g_dg_rb) \
                            && (g_dg_r1 < 0 || (uint32_t)R[1] == (uint32_t)g_dg_r1) \
                            && (g_dg_sp < 0 || (uint32_t)R[CVM_REG_SP] == (uint32_t)g_dg_sp)) \
                            dg_pcdump((pc) - 1u, op, a, b, c, R, (uint32_t)R[CVM_REG_SP], heap, mem_size); \
                          if (trip_pc != 0xffffffffu && (pc) - 1u == trip_pc \
                              && (trip_sp == 0 || (uint32_t)R[CVM_REG_SP] == trip_sp)) \
                              trip_rb = (uint32_t)R[b]; } while (0)
#else
#  define DG_W(addr, sz, val) ((void)0)
#  define MISP_SCAN(dst, src, len) ((void)0)
#  define DG_HOOK()           ((void)0)
#  define RZ_CALL(fid)        ((void)0)
#  define RZ_RET()            ((void)0)
#  define RZ_CHECK(addr, sz)  ((void)0)
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
#if defined(CVM_DIAG)
    dg_init();
    { static int once = 0; if (!once) { once = 1;
        fprintf(stderr, "[DIAG] mem_size=0x%08x heap_size=0x%08x reserve=0x%08x heap_start=0x%08x\n",
                img->mem_size, img->heap_size, img->reserve_size,
                (uint32_t)(img->heap_size - img->reserve_size)); fflush(stderr); } }
    rz_setup((uint32_t)(img->heap_size - img->reserve_size), img->heap_size,
             img->func_offsets, img->func_count);
    g_hs = (uint32_t)(img->heap_size - img->reserve_size); g_he = img->heap_size;
    if (getenv("CVM_REDZONE") && !lw_pc) {       /* cover heap+stack = [heap_start, mem_size) */
        lw_base  = (uint32_t)(img->heap_size - img->reserve_size);
        lw_words = (img->mem_size - lw_base) >> 2;
        lw_pc    = (uint32_t *)calloc(lw_words ? lw_words : 1u, 4);
    }
#endif

#ifdef CVM_PROFILE
    /* Self-time attribution by the CURRENT PC (not a shadow call stack). The fid
     * is the function whose [entry, next-entry) range contains pc, found by binary
     * search over the ascending func_offsets and cached in [prof_lo, prof_hi) so the
     * search only runs when execution crosses a function boundary. This is immune to
     * non-local control flow — longjmp (C++ throw) and coro_swap just move pc, and
     * the next tick re-derives the right fid — which the old shadow stack could not
     * track (it desynced on every throw and coroutine switch, sticking prof_cur on a
     * fixed wrong fid). */
    uint32_t prof_cur = 0, prof_lo = 0, prof_hi = 0;  /* current fid + its pc range */
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
        [CVM_OP_CORO_SWAP] = &&L_CORO_SWAP,
        [CVM_OP_FFLOOR]  = &&L_FFLOOR,
        [CVM_OP_FCEIL]   = &&L_FCEIL,
        [CVM_OP_FTRUNC]  = &&L_FTRUNC,
    };

#  define DISPATCH() do {                                  \
        if (pc >= code_count) return CVM_E_BAD_PC;         \
        PROF_TICK();                                       \
        inst = code[pc++];                                 \
        op = (uint8_t)(inst & 0xFFu);                      \
        a  = (uint8_t)((inst >>  8) & 0xFFu);              \
        b  = (uint8_t)((inst >> 16) & 0xFFu);              \
        c  = (uint8_t)((inst >> 24) & 0xFFu);              \
        DG_HOOK();                                         \
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
        DG_W(addr, 4u, v);
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
        RZ_CALL(fid);
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
        RZ_RET();
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
        RZ_CALL(fid);
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
        DG_W(addr, 1u, (uint32_t)R[c] & 0xFFu);
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
        DG_W(addr, 2u, v);
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
            DG_W(dst, len, src);
            MISP_SCAN(dst, src, len);
            memcpy(heap + dst, heap + src, len);
        }
        DISPATCH();
    }
    L_MEMSET: {
        uint32_t dst = (uint32_t)R[a];
        uint32_t len = (uint32_t)R[c];
        if (len) {
            if (dst > mem_size || mem_size - dst < len) return CVM_E_BAD_ADDR;
            DG_W(dst, len, (uint32_t)R[b] & 0xFFu);
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
            DG_W(dst, len, src);
            MISP_SCAN(dst, src, len);
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
    L_FFLOOR:
        R[a] = cvm_f32_to_bits(floorf(cvm_bits_to_f32(R[b])));
        DISPATCH();
    L_FCEIL:
        R[a] = cvm_f32_to_bits(ceilf(cvm_bits_to_f32(R[b])));
        DISPATCH();
    L_FTRUNC:
        R[a] = cvm_f32_to_bits(truncf(cvm_bits_to_f32(R[b])));
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
        PROF_LONGJMP(jsp);
        DISPATCH();
    }
    L_CORO_SWAP: {
        uint32_t from = (uint32_t)R[a];
        uint32_t to   = (uint32_t)R[b];
        if (from > mem_size || mem_size - from < 16u) return CVM_E_BAD_ADDR;
        if (to   > mem_size || mem_size - to   < 16u) return CVM_E_BAD_ADDR;
        if (from == to) return CVM_E_BAD_ADDR;       /* self-swap = bug */
        uint32_t jstat;
        memcpy(&jstat, heap + to + 12u, 4);
        if (jstat != 0u && jstat != 2u) return CVM_E_BAD_CORO_STATE;
        uint32_t jpc, jsp, jdst;
        memcpy(&jpc,  heap + to + 0u, 4);
        memcpy(&jsp,  heap + to + 4u, 4);
        memcpy(&jdst, heap + to + 8u, 4);
        if (jstat == 0u) {
            /* CORO_FRESH — jpc is a function index, FUNCS lookup; jdst is
             * where the entry fn expects its arg0 (ABI: R0). */
            if (jpc == 0u)              return CVM_E_NULL_FUNC_PTR;
            if (jpc >= func_count)      return CVM_E_BAD_FUNC_INDEX;
            jpc = func_offsets[jpc];
            if (jdst >= CVM_REG_COUNT)  return CVM_E_BAD_ADDR;
        }
        if (jpc >= code_count)         return CVM_E_BAD_PC;
        uint32_t cur_sp     = (uint32_t)R[CVM_REG_SP];
        uint32_t dest_a     = a;
        uint32_t suspended  = 2u;
        uint32_t running    = 1u;
        memcpy(heap + from +  0u, &pc,        4);
        memcpy(heap + from +  4u, &cur_sp,    4);
        memcpy(heap + from +  8u, &dest_a,    4);
        memcpy(heap + from + 12u, &suspended, 4);
        memcpy(heap + to   + 12u, &running,   4);
        /* FRESH resume: hand the new coroutine pointer (`to`) to the entry
         * fn in its arg0 reg. The trampoline / user-supplied entry takes
         * `cron_coro_t *self` so it can read self->fn(self->arg). The swap
         * initiator is still recoverable as to->resumer (the cart-side
         * cron_coro_swap wrapper sets it just before the opcode fires). */
        /* SUSPENDED resume: no register clobber — caller-saved regs are
         * preserved across the swap, no returns_twice needed at the call
         * site. */
        if (jstat == 0u) R[jdst] = (int32_t)to;
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
        case CVM_OP_FFLOOR:
            R[a] = cvm_f32_to_bits(floorf(cvm_bits_to_f32(R[b])));
            break;
        case CVM_OP_FCEIL:
            R[a] = cvm_f32_to_bits(ceilf(cvm_bits_to_f32(R[b])));
            break;
        case CVM_OP_FTRUNC:
            R[a] = cvm_f32_to_bits(truncf(cvm_bits_to_f32(R[b])));
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
            PROF_LONGJMP(jsp);
            break;
        }
        case CVM_OP_CORO_SWAP: {
            uint32_t from = (uint32_t)R[a];
            uint32_t to   = (uint32_t)R[b];
            if (from > mem_size || mem_size - from < 16u) return CVM_E_BAD_ADDR;
            if (to   > mem_size || mem_size - to   < 16u) return CVM_E_BAD_ADDR;
            if (from == to) return CVM_E_BAD_ADDR;
            uint32_t jstat;
            memcpy(&jstat, heap + to + 12u, 4);
            if (jstat != 0u && jstat != 2u) return CVM_E_BAD_CORO_STATE;
            uint32_t jpc, jsp, jdst;
            memcpy(&jpc,  heap + to + 0u, 4);
            memcpy(&jsp,  heap + to + 4u, 4);
            memcpy(&jdst, heap + to + 8u, 4);
            if (jstat == 0u) {
                if (jpc == 0u)              return CVM_E_NULL_FUNC_PTR;
                if (jpc >= func_count)      return CVM_E_BAD_FUNC_INDEX;
                jpc = func_offsets[jpc];
                if (jdst >= CVM_REG_COUNT)  return CVM_E_BAD_ADDR;
            }
            if (jpc >= code_count)         return CVM_E_BAD_PC;
            uint32_t cur_sp     = (uint32_t)R[CVM_REG_SP];
            uint32_t dest_a     = a;
            uint32_t suspended  = 2u;
            uint32_t running    = 1u;
            memcpy(heap + from +  0u, &pc,        4);
            memcpy(heap + from +  4u, &cur_sp,    4);
            memcpy(heap + from +  8u, &dest_a,    4);
            memcpy(heap + from + 12u, &suspended, 4);
            memcpy(heap + to   + 12u, &running,   4);
            if (jstat == 0u) R[jdst] = (int32_t)to;
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
