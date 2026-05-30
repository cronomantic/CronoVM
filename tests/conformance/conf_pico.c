/* conf_pico.c — conformance slice: the picolibc C-library surface.
 *
 * Unlike the other fixtures (which pin a translator construct), this one links
 * the real picolibc bitcode (runtime/lib/picolibc.bc) on the VM side and the
 * host libc on the native side, then checks that picolibc's string/stdlib
 * routines produce byte-identical results to the host's. It is both a smoke
 * test that picolibc.bc translates+runs and a differential check of its logic.
 *
 * Discipline (same as the rest of the corpus): everything folds into one int32
 * checksum, and every libc result is cast to a fixed width before mixing, so
 * host-long-64 and VM-long-32 agree bit-for-bit (all values stay in int range).
 *
 * The VM build gets malloc/free + main from vm_entry.c and errno from
 * pico_machine.c (the minimal machine port); the native build uses host libc.
 */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

static int NOINLINE cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)
    /* strcmp/strncmp/memcmp only guarantee the SIGN of their result, not the
     * magnitude (host returns the byte difference; picolibc returns -1/0/1).
     * Normalise to {-1,0,1} so the differential checksum compares semantics. */
    #define SGN(x) (((x) > 0) - ((x) < 0))

    /* ---- string.h: lengths, compares, searches ---- */
    const char *s = "picolibc on CronoVM";
    MIX(strlen(s));
    MIX(SGN(strcmp(s, "picolibc on CronoVM")));
    MIX(SGN(strcmp(s, "picolibc on cronovm")));   /* case differs -> sign */
    MIX(SGN(strncmp(s, "picolibc XX", 8)));
    MIX(strchr(s, 'o') - s);
    MIX(strrchr(s, 'o') - s);
    MIX(strstr(s, "Crono") - s);
    MIX(strstr(s, "absent") == NULL);
    MIX(strspn("aabbccd", "abc"));
    const char *hw = "hello world";
    MIX(strcspn(hw, " "));
    MIX(strpbrk(hw, "wxyz") - hw);     /* same object — well-defined diff */

    char buf[64];
    strcpy(buf, "abc");
    strcat(buf, "def");
    MIX(strlen(buf));
    MIX(SGN(memcmp(buf, "abcdef", 6)));
    MIX(SGN(memcmp("abcdef", "abcdeg", 6)));   /* differing -> sign */
    strncpy(buf, "0123456789", 5); buf[5] = 0;
    MIX(strlen(buf)); MIX(buf[4]);

    /* ---- string.h: mem* ---- */
    uint8_t mb[32];
    memset(mb, 0x5A, sizeof mb);
    MIX(mb[0]); MIX(mb[31]);
    uint8_t src[16];
    for (int i = 0; i < 16; ++i) src[i] = (uint8_t)(i * 3 + 1);
    memcpy(mb, src, 16);
    for (int i = 0; i < 16; ++i) MIX(mb[i]);
    memmove(mb + 4, mb, 12);          /* overlapping */
    for (int i = 0; i < 32; ++i) MIX(mb[i]);
    MIX((const uint8_t *)memchr(src, 3 * 5 + 1, 16) - src);

    /* ---- stdlib.h: integer ---- */
    MIX(abs(-12345));
    MIX(labs(-987654L));
    div_t d = div(100, 7);   MIX(d.quot); MIX(d.rem);
    ldiv_t l = ldiv(-100L, 7L); MIX((int)l.quot); MIX((int)l.rem);
    MIX(atoi("  -42abc"));
    MIX((int)atol("1000000"));

    /* ---- numeric parsing (touches errno path; in-range so errno untouched) ---- */
    MIX((int)strtol("  -2147483", NULL, 10));
    MIX((int)strtol("7fff", NULL, 16));
    MIX((int)strtol("-0", NULL, 10));
    MIX((int)strtoul("4294967", NULL, 10));
    MIX((int)strtoul("deadBEEF", NULL, 16));
    /* 64-bit: strtoull + llabs exercise the i64 surface (with.overflow + abs.i64) */
    unsigned long long u64 = strtoull("12345678901234", NULL, 10);
    MIX((int32_t)u64); MIX((int32_t)(u64 >> 32));
    long long ll = llabs(-9000000000LL);
    MIX((int32_t)ll); MIX((int32_t)(ll >> 32));

    /* ---- qsort + bsearch (function pointers + maybe malloc) ---- */
    int arr[] = { 9, 3, 7, 1, 8, 2, 6, 0, 5, 4 };
    int n = (int)(sizeof arr / sizeof arr[0]);
    qsort(arr, (size_t)n, sizeof arr[0], cmp_int);
    for (int i = 0; i < n; ++i) MIX(arr[i]);
    int key = 6;
    int *found = (int *)bsearch(&key, arr, (size_t)n, sizeof arr[0], cmp_int);
    MIX(found ? *found : -1);
    MIX(found - arr);

    /* ---- the CANONICAL allocator: malloc/free/calloc/realloc ----
     * picolibc owns these on the VM side (backed by sbrk in pico_machine.c); the
     * native oracle uses host libc. Addresses differ, so checksum only OBSERVABLE
     * behaviour — bytes written/read, zeroing, content preserved across realloc,
     * NULL on overflow — never a returned pointer value. */
    unsigned char *p = (unsigned char *)malloc(100);
    MIX(p != NULL);
    for (int i = 0; i < 100; ++i) p[i] = (unsigned char)(i * 7 + 3);
    for (int i = 0; i < 100; ++i) MIX(p[i]);              /* survives the write */

    /* realloc grows + preserves the first 100 bytes */
    unsigned char *q = (unsigned char *)realloc(p, 300);
    MIX(q != NULL);
    for (int i = 0; i < 100; ++i) MIX(q[i]);              /* preserved content */
    for (int i = 100; i < 300; ++i) q[i] = (unsigned char)(255 - (i & 0xFF));
    MIX(q[150]); MIX(q[299]);

    /* realloc shrink, then free */
    unsigned char *r = (unsigned char *)realloc(q, 50);
    MIX(r != NULL);
    for (int i = 0; i < 50; ++i) MIX(r[i]);
    free(r);

    /* calloc zeroes its block */
    int *z = (int *)calloc(32, sizeof(int));
    MIX(z != NULL);
    int zsum = 0;
    for (int i = 0; i < 32; ++i) zsum |= z[i];            /* must stay 0 */
    MIX(zsum);
    z[0] = 0x1234; z[31] = 0x5678;
    MIX(z[0]); MIX(z[31]);
    free(z);
    /* NOTE: the calloc nmemb*size overflow guard is deliberately NOT tested
     * differentially — picolibc detects the wrap and returns NULL, but some
     * host libcs (the native oracle) don't, and size_t is 32-bit on the VM vs
     * 64-bit on the host, so the edge case isn't comparable. picolibc's guard
     * is exercised by its own test suite. */

    /* a churn loop: alloc/fill/free repeatedly, fold the readback */
    for (int k = 0; k < 8; ++k) {
        size_t sz = (size_t)(16 + k * 9);
        unsigned char *b = (unsigned char *)malloc(sz);
        MIX(b != NULL);
        for (size_t i = 0; i < sz; ++i) b[i] = (unsigned char)(i ^ (k * 13));
        MIX(b[0]); MIX(b[sz - 1]);
        free(b);
    }

    #undef SGN
    #undef MIX
    return (int)h;
}
