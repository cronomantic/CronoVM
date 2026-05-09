/* Multi-function fixture: vm_main calls add and adds 1 to the result.
 * `noinline` on add prevents -O1 from folding it into vm_main, so we
 * actually exercise CALL/RET. The translator picks the first definition
 * as the entry point when no `main` is present, so vm_main goes first. */

__attribute__((noinline))
static int add(int a, int b);

int vm_main(int x, int y) {
    return add(x, y) + 1;
}

__attribute__((noinline))
static int add(int a, int b) {
    return a + b;
}
