/* Exercises sub-word memory ops added in step 7a.
 *
 *   ub[]:  unsigned char load  → LDB + ZExt (no-op MOV)
 *   sb[]:  signed   char load  → LDB + SExt (shift-pair)
 *   uh[]:  unsigned short load → LDH + ZExt (no-op MOV)
 *   sh[]:  signed   short load → LDH + SExt (shift-pair)
 *   scratch[]: STB round-trip — write low byte of accumulator, read it back
 *   word16:    STH round-trip — write low halfword, read it back
 *
 * Expected for narrow_main(8):
 *   ub : 10+20+30+40+50+60+70+80      =  360
 *   sb : -1-2-3-4                     =  -10
 *   uh : 1000+2000+3000+4000          = 10000
 *   sh : -100-200+30000-32000         = -2300
 *   s = 360 - 10 + 10000 - 2300       = 8050  (= 0x1F72)
 *   scratch[0] = (uint8_t) s          = 0x72  = 114
 *   word16     = (uint16_t)s          = 0x1F72 = 8050  (read back as unsigned)
 *   return s + scratch[0] + word16    = 8050 + 114 + 8050 = 16214
 */

unsigned char  ub[8] = {10, 20, 30, 40, 50, 60, 70, 80};
signed   char  sb[4] = {-1, -2, -3, -4};
unsigned short uh[4] = {1000, 2000, 3000, 4000};
signed   short sh[4] = {-100, -200, 30000, -32000};

unsigned char  scratch[4] = {0, 0, 0, 0};
unsigned short word16     = 0;

int narrow_main(int n) {
    int s = 0;
    for (int i = 0; i < n;  i++) s += ub[i];
    for (int i = 0; i < 4;  i++) s += sb[i];
    for (int i = 0; i < 4;  i++) s += uh[i];
    for (int i = 0; i < 4;  i++) s += sh[i];

    scratch[0] = (unsigned char) s;
    word16     = (unsigned short)s;

    return s + scratch[0] + word16;
}
