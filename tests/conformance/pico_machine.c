/* pico_machine.c — the minimal CronoVM machine port for the picolibc
 * conformance fixture (conf_pico.c).
 *
 * picolibc.bc is the pure, embedder-independent C surface; it leaves a tiny set
 * of symbols for the embedder to supply (see build_picolibc.sh's reported
 * "machine-port surface"). vm_entry.c already provides malloc/free; the only
 * other symbol picolibc's strtol/strtoul reference is the global errno.
 *
 * A real cart gets all of this from the Cronopio SDK libc (mapped to cron
 * syscalls). This file is just enough for the standalone VM test to link. */

/* __GLOBAL_ERRNO: a single global int, defined here, declared `extern int errno`
 * by picolibc's <errno.h>. */
int errno;
