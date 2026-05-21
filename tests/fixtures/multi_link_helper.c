/* Multi-file link fixture (part 2 of 2). See multi_link_main.c. The `bias`
 * here shares its name with the one in multi_link_main.c but holds a different
 * value; the two must not collide after llvm-link. */
static int bias = 100;

int twice(int x) {
    return x * 2 + bias;
}
