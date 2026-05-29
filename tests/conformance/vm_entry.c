/* vm_entry.c — VM-side entry shim for the conformance corpus. The fixtures
 * export `conf_main` (not `main`), so the translator's "named main, else first
 * defined function" entry heuristic could pick the wrong function in a
 * multi-function module (notably a C++ global-ctor function). Providing a real
 * `main` that tail-calls conf_main pins the entry deterministically for every
 * fixture, C and C++ alike. (Real carts already name their entry `main`.) */
extern int conf_main(void);
int main(void) { return conf_main(); }
