/* Multi-file link fixture (part 1 of 2). The entry point vm_main lives here
 * and calls `twice`, which is defined in multi_link_helper.c — exercising
 * cvm-cc's clang-per-file + llvm-link path. Both files define a file-local
 * `bias` with the SAME name and a DIFFERENT value; llvm-link must keep them
 * distinct (per-TU static linkage), not merge them. An amalgamation (one .c
 * #including both) would fail to compile on the duplicate definition.
 *
 * vm_main(x,y) = twice(x) + y + bias = (x*2 + 100) + y + 1 = 2x + y + 101. */
static int bias = 1;
int twice(int x);                 /* defined in multi_link_helper.c */

int vm_main(int x, int y) {
    return twice(x) + y + bias;
}
