/* Differential conformance slice: picolibc FLOAT transcendentals.
 *
 * Guards sinf/cosf/powf/logf — picolibc's single-precision libm, added to
 * picolibc.bc (NOT single VM opcodes, unlike sqrtf/floorf in conf_float_libcall).
 *
 * Transcendentals are NOT bit-exact across implementations: on the VM they are
 * picolibc's float libm, while the native oracle links the HOST libm, and the two
 * differ at the ULP. So a bit-exact FNV checksum (the usual fixture style) would
 * fail spuriously. Instead this fixture returns the COUNT of results that land
 * within a generous tolerance of known exact values — both the VM and the native
 * oracle pass every check, so both return the same count. That is differential-
 * safe AND proves the functions link + compute correctly.
 *
 * conf_pico_* prefix -> the runner links the real picolibc bitcode on the VM side.
 */
#include <stdint.h>

extern float sinf(float);
extern float cosf(float);
extern float powf(float, float);
extern float logf(float);
extern float fabsf(float);

static int near(float a, float b) {
    return fabsf(a - b) < 0.01f;
}

int conf_main(void) {
    int pass = 0;

    /* sinf */
    if (near(sinf(0.0f), 0.0f)) pass++;
    if (near(sinf(1.5707963f), 1.0f)) pass++;    /* pi/2  -> 1   */
    if (near(sinf(0.5235988f), 0.5f)) pass++;    /* pi/6  -> 0.5 */

    /* cosf */
    if (near(cosf(0.0f), 1.0f)) pass++;
    if (near(cosf(1.5707963f), 0.0f)) pass++;    /* pi/2  -> 0   */
    if (near(cosf(1.0471976f), 0.5f)) pass++;    /* pi/3  -> 0.5 */

    /* powf */
    if (near(powf(2.0f, 10.0f), 1024.0f)) pass++;
    if (near(powf(9.0f, 0.5f), 3.0f)) pass++;    /* sqrt via pow */
    if (near(powf(5.0f, 0.0f), 1.0f)) pass++;

    /* logf */
    if (near(logf(1.0f), 0.0f)) pass++;
    if (near(logf(2.7182818f), 1.0f)) pass++;    /* e   -> 1 */
    if (near(logf(7.389056f), 2.0f)) pass++;     /* e^2 -> 2 */

    return pass;    /* 12 on both the VM and the native oracle */
}
