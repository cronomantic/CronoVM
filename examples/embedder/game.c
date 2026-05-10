/* Minimal "game" code compiled by cvm-cc into game.bin. The wrapper
 * picks the first defined function as the entry point; with no args
 * seeded by the host, R0 is 0 at start and we return a known value
 * that the host can verify. */

int game(int n) {
    return n + 42;
}
