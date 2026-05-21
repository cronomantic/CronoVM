/* Regression fixture for the block-local register-reuse corruption.
 *
 * In a loop, a select result and the GEP address it feeds — plus the loop
 * counter — were all assigned the same physical register: cg_alloc_reg drew
 * `dst` from the free pool onto an operand's register, then freeing that
 * operand re-pooled the (now-live) dst register, so the next allocation
 * clobbered it. The store wrote address-into-itself and the loop counter was
 * destroyed by the trip-count compare -> infinite loop.
 *
 * The trigger is dx=x-8 (a negative add) feeding dx*dx, an if/else-if chain
 * (which clang lowers to nested selects), and a store to a live global array.
 * vm_main(n) fills out[256] with a radial pattern and returns out[n]:
 *   n=136 (x=8,y=8) -> r2=0   -> 8
 *   n=0   (x=0,y=0) -> r2=128 -> 0
 */

unsigned char out[256];

int vm_main(int n) {
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            int dx = x - 8, dy = y - 8;
            int r2 = dx*dx + dy*dy;
            unsigned char c = 0;
            if (r2 < 49) c = 8; else if (r2 < 64) c = 7;
            out[y*16 + x] = c;
        }
    return out[n];
}
