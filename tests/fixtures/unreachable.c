/* Exercises the LLVM `unreachable` terminator lowering added for the
 * DOOM port. clang -O1 places a bare `unreachable` after a `noreturn`
 * call (DOOM does this after I_Error). The translator lowers it to a
 * defensive HALT.
 *
 * my_die is noreturn (an infinite loop), so the call site is followed by
 * `unreachable`. vm_main(7) takes the a>=0 path and never calls it, so
 * the test returns 14 — proving the function with the `unreachable`
 * terminator translates and runs correctly on the live path. */
__attribute__((noreturn, noinline)) static void my_die(void)
{
    /* volatile so the compiler keeps the loop and the call site, leaving
     * a real `unreachable` terminator after the call in vm_main. */
    volatile int spin = 1;
    while (spin) { }
    __builtin_unreachable();
}

int vm_main(int a)
{
    if (a < 0)
        my_die();          /* `unreachable` terminator after this call */
    return a * 2;
}
