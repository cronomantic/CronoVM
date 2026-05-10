/* Validates the host-region mechanism end-to-end.
 *
 *   Region "input" (4 bytes, host writes): the host stores a single i32
 *   value `v` here before calling cvm_run.
 *
 *   Region "fb" (16 bytes, VM writes): the fixture fills it with the
 *   pattern `fb[i] = (uint8_t)(i * v)`. The host harness verifies the
 *   bytes match after the run completes.
 *
 * Returns 0 on success, otherwise a non-zero code identifying which
 * lookup failed. */

extern int cvm_sys_get_region(const char *name);

int regions_main(int n) {
    (void)n;
    int in_off = cvm_sys_get_region("input");
    if (in_off < 0) return 1;
    int fb_off = cvm_sys_get_region("fb");
    if (fb_off < 0) return 2;

    /* VM heap addresses are 32-bit byte offsets; cast int → ptr for
     * direct LDW/STB access. The translator lowers this to a register
     * MOV followed by the appropriate memory op. */
    int           *in = (int *)in_off;
    unsigned char *fb = (unsigned char *)fb_off;

    int v = *in;
    for (int i = 0; i < 16; i++) {
        fb[i] = (unsigned char)(i * v);
    }
    return 0;
}
