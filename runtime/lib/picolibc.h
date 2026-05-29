/* picolibc.h — hand-written CronoVM configuration for picolibc.
 *
 * Picolibc normally generates this header from picolibc.h.in via meson/cmake.
 * CronoVM does NOT drive picolibc's build system; it compiles a curated subset
 * of picolibc sources straight to i386-elf LLVM bitcode (see build_picolibc.sh)
 * with this header force-found on the include path. So this file is the single
 * point that pins every picolibc feature knob for the VM.
 *
 * Pinned to picolibc 1.8.11 (newlib 4.3.0), submodule external/picolibc.
 * When the submodule pin moves, re-check this against picolibc.h.in for new or
 * renamed cmakedefine knobs.
 *
 * Design (mirrors the libc/libc++ decision):
 *   - FREESTANDING: no hosted OS. The OS hooks picolibc calls (write/read/sbrk/
 *     exit/...) are supplied by the EMBEDDER's machine port (Cronopio's SDK libc,
 *     mapped to cron syscalls), not by picolibc.
 *   - SINGLE-CONTEXT libc. The VM is cooperative (CORO_SWAP, no preemption) and
 *     no libc call yields mid-way, so a global errno + a single-threaded malloc
 *     are safe — matching the old SDK libc. (libc++'s std::thread/mutex use the
 *     EXTERNAL thread API over cron-coros; that is a libc++ concern, not picolibc.)
 *   - NO locale / wide chars / multibyte / filesystem in the C surface.
 */

#pragma once

/* ---- Version identity (unconditional #defines in picolibc.h.in) ---------- */
#define __PICOLIBC__            1
#define __PICOLIBC_MINOR__      8
#define __PICOLIBC_PATCHLEVEL__ 11
#define __PICOLIBC_VERSION__    "1.8.11"
/* Legacy single-underscore spellings some headers still read. */
#define _PICOLIBC__             1
#define _PICOLIBC_MINOR__       8
#define _PICOLIBC_VERSION       "1.8.11"

#define __NEWLIB__              4
#define __NEWLIB_MINOR__        3
#define __NEWLIB_PATCHLEVEL__   0
#define _NEWLIB_VERSION         "4.3.0"

/* ---- Core feature selection ---------------------------------------------- */

/* Tiny stdio (the small, self-contained printf/scanf/FILE core) rather than
 * the large newlib stdio. */
#define __TINY_STDIO

/* Single global `errno` (no TLS). Safe under cooperative scheduling. */
#define __GLOBAL_ERRNO

/* picolibc itself runs in one context (locks become no-ops). The cooperative
 * scheduler never preempts inside a libc call. */
#define __SINGLE_THREAD

/* SAFETY-CRITICAL for the bounded VM: force the byte-at-a-time string routines.
 * The speed-optimised strlen/memchr/... read a full word at a time and
 * deliberately over-read past the end of the buffer up to word alignment. In
 * CronoVM's bounds-checked address space that over-read can trap at a region or
 * heap boundary. __PREFER_SIZE_OVER_SPEED keeps every access in-bounds.
 * (It is also smaller, which suits the VM.) */
#define __PREFER_SIZE_OVER_SPEED

/* ---- stdio formatting variant -------------------------------------------- */
/* Default printf/scanf conversion family. 'd' = full double formatting (the
 * engines use %f); the f64 soft runtime is auto-linked when needed.
 * 'i'=integer-only, 'f'=float, 'l'=long long. */
#define __IO_DEFAULT 'd'

/* ---- Math implementation selection (unconditional #defines) -------------- */
/* 0 = modern math sources. Only consulted when libm sources are compiled
 * (a later phase); pinned here so any transitive math header compiles. */
#define __OBSOLETE_MATH_FLOAT  0
#define __OBSOLETE_MATH_DOUBLE 0
/* libm offers IEEE semantics without touching errno (no errno coupling in the
 * hot math paths). Revisit if a cart needs math_errhandling. */
#define __IEEE_LIBM

/* ---- Deliberately OFF (left undefined) -----------------------------------
 * __THREAD_LOCAL_STORAGE*, __THREAD_LOCAL_STORAGE_API   (single-context libc)
 * __SEMIHOST, __PICOCRT_ENABLE_MMU, __PICOCRT_RUNTIME_SIZE (no picocrt/host)
 * __INIT_FINI_ARRAY, __INIT_FINI_FUNCS, _WANT_REGISTER_FINI,
 *   _ATEXIT_DYNAMIC_ALLOC, _GLOBAL_ATEXIT  (entry runs llvm.global_ctors via
 *                                           the translator; no crt _init/_fini)
 * __MB_CAPABLE, __MB_EXTENDED_CHARSETS_*, __IO_WCHAR, __WIDE_ORIENT (no wide/MB)
 * __HAVE_COMPLEX                                          (no _Complex)
 * __HAVE_FCNTL                                  (machine port has no fcntl yet)
 * __IO_C99_FORMATS, __IO_LONG_LONG, __IO_LONG_DOUBLE, __IO_POS_ARGS,
 *   __IO_FLOAT_EXACT, __NANO_FORMATTED_IO    (minimal stdio; widen on demand)
 * __NANO_MALLOC, __MALLOC_SMALL_BUCKET                  (allocator phase)
 * __FAST_STRCMP, __ATOMIC_UNGETC, POSIX_CONSOLE, __ASSERT_VERBOSE
 * __ELIX_LEVEL  (unused by the string/stdlib/stdio sources we compile)
 * --------------------------------------------------------------------------- */
