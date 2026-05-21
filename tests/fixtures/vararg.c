/* Variadic-function fixture. Exercises va_start/va_arg/va_end lowering: the
 * entry vm_main calls a variadic vsum with named + unnamed args. vsum is
 * forward-declared so vm_main is the first definition (the entry point).
 * vm_main(a,b) = vsum(6, a,b,100,1,2,3) = a + b + 106. With a=7,b=5 -> 118. */
#include <stdarg.h>

static int vsum(int n, ...);

int vm_main(int a, int b) {
    return vsum(6, a, b, 100, 1, 2, 3);
}

__attribute__((noinline))
static int vsum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}
