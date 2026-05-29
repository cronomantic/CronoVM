/* driver.c — native oracle driver for the differential conformance corpus.
 *
 * Each conformance fixture exposes `int conf_main(void)` that exercises a slice
 * of the C / LLVM-intrinsic surface and folds the results into one int32
 * checksum. This driver builds NATIVELY (host clang) and prints that checksum;
 * the runner then runs the SAME fixture on the VM (cvm-cc) and compares. A
 * mismatch is a miscompile; a translate failure is an unlowered gap.
 *
 * Printed as a signed decimal so it round-trips through test_e2e's strtol. */
#include <stdio.h>

extern int conf_main(void);

int main(void) {
    printf("%d\n", conf_main());
    return 0;
}
