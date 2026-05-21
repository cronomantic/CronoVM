/* Exercises narrow funnel-shift lowerings added for the DOOM port:
 * llvm.fshl.i8 and llvm.fshr.i8 (clang -O1 emits these for 8-bit
 * rotates, e.g. __builtin_rotateleft8 / __builtin_rotateright8).
 *
 * For a=10 (x=0x0A=0b00001010):
 *   rotl8(x,3) = 0b01010000 = 0x50 = 80   (llvm.fshl.i8)
 *   rotr8(x,2) = 0b10000010 = 0x82 = 130  (llvm.fshr.i8)
 * Result = 80*1000 + 130 = 80130.
 */
static unsigned char rotl8(unsigned char v, unsigned char n)
{
    return __builtin_rotateleft8(v, n);
}
static unsigned char rotr8(unsigned char v, unsigned char n)
{
    return __builtin_rotateright8(v, n);
}

int vm_main(int a)
{
    unsigned char x = (unsigned char)a;
    int l = rotl8(x, 3);   /* llvm.fshl.i8(x, x, 3) */
    int r = rotr8(x, 2);   /* llvm.fshr.i8(x, x, 2) */
    return l * 1000 + r;
}
