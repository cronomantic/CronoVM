/* Indirect call via function pointer. With both candidates noinline,
 * Clang -O1 lowers `sel ? add1 : sub1` as a `select` between two
 * function pointers followed by an indirect call — exactly the path
 * that exercises CALLR. */

__attribute__((noinline))
static int add1(int x) { return x + 1; }

__attribute__((noinline))
static int sub1(int x) { return x - 1; }

int vm_main(int sel, int x) {
    int (*fp)(int) = sel ? add1 : sub1;
    return fp(x);
}
