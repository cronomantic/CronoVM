/* Recursive fib via real CALL/RET. `noinline` keeps -O1 from collapsing
 * the recursion. The translator picks the first definition as the entry
 * point when no `main` is present. */

__attribute__((noinline))
static int fib(int n);

int vm_main(int n) {
    return fib(n);
}

__attribute__((noinline))
static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
