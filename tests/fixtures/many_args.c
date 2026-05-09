/* Exercises the stacked-arg path of the calling convention: more than 8
 * parameters means a, b, c, d, e, f, g, h come in R0..R7 and i, j arrive
 * via the caller-pushed stack region. */

__attribute__((noinline))
static int sum10(int a, int b, int c, int d, int e,
                 int f, int g, int h, int i, int j);

int vm_main(int x, int y) {
    return sum10(x, y, 1, 2, 3, 4, 5, 6, 7, 8);
}

__attribute__((noinline))
static int sum10(int a, int b, int c, int d, int e,
                 int f, int g, int h, int i, int j) {
    return a + b + c + d + e + f + g + h + i + j;
}
