int bitops(int a, int b) {
    int x = a << 4;     /* shl */
    int y = b >> 1;     /* ashr (signed) */
    int z = a & b;      /* and */
    int w = a | b;      /* or  */
    int v = a ^ b;      /* xor */
    return x + y + z + w + v;
}
