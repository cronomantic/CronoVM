/* alloca survives -O1 only when its address escapes — passing a pointer
 * to a noinline function does the trick. swap reads/writes the two slots,
 * exercising the alloca-pointer materialisation in the prologue. */

__attribute__((noinline))
static void swap(int *p, int *q) {
    int t = *p;
    *p = *q;
    *q = t;
}

int vm_main(int x, int y) {
    int a = x;
    int b = y;
    swap(&a, &b);
    return a - b;   /* original y - original x */
}
