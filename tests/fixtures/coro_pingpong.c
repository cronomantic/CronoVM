/* Coroutine ping-pong — three coros bouncing a counter via cron_coro_yield
 * (which is cron_coro_swap(self, self->resumer)).  Exercises:
 *   - multiple CORO_FRESH initial swaps
 *   - cart-side resumer chain tracking
 *   - many SUSPENDED-resume context restores without losing live SSA regs
 *
 * Local declarations mirror the SDK so the fixture stays freestanding.
 *
 * Run: main creates A and B; main swaps to A; A bumps then swaps to B; B
 * bumps then swaps back to A (its resumer); A bumps again then swaps back to
 * main (its resumer). main reads the counter and returns counter + n.
 * Counter ends at 3, vm_main(5) -> 8, vm_main(0) -> 3. */

#include <stdint.h>

typedef struct cron_coro {
    uint32_t _pc;
    uint32_t _sp;
    uint32_t _dest;
    uint32_t status;
    void   (*fn)(void *);
    void    *arg;
    void    *stack_lo;
    uint32_t stack_sz;
    struct cron_coro *resumer;
} cron_coro_t;

extern void __cvm_coro_swap_raw(cron_coro_t *from, cron_coro_t *to)
    __attribute__((__returns_twice__));

static cron_coro_t main_coro;
static cron_coro_t a_coro, b_coro;
static uint8_t a_stack[8192], b_stack[8192];
static int counter;

/* Wrappers that set resumer cart-side then invoke the opcode. Same as the
 * SDK's cron_coro_swap. */
static inline void cron_coro_swap(cron_coro_t *from, cron_coro_t *to) {
    to->resumer = from;
    __cvm_coro_swap_raw(from, to);
}

static void a_fn(cron_coro_t *self) {
    /* Capture the FIRST resumer (main) — self->resumer gets overwritten
     * when b later swaps back to us. */
    cron_coro_t *main_caller = self->resumer;
    counter++;                       /* 1 */
    cron_coro_swap(self, &b_coro);   /* a -> b */
    counter++;                       /* 3, after b returns to us */
    cron_coro_swap(self, main_caller);
}

static void b_fn(cron_coro_t *self) {
    counter++;                       /* 2 */
    cron_coro_swap(self, self->resumer);  /* b -> a */
}

static void init_coro(cron_coro_t *c, void (*fn)(void *), uint8_t *stack, uint32_t stack_sz) {
    uint8_t *top = stack + stack_sz - 4;
    *(uint32_t *)top = 0xFFFFFFFEu;
    c->_pc    = (uint32_t)(uintptr_t)fn;
    c->_sp    = (uint32_t)(uintptr_t)top;
    c->_dest  = 0;
    c->status = 0;
}

int vm_main(int n) {
    init_coro(&a_coro, (void (*)(void *))a_fn, a_stack, sizeof a_stack);
    init_coro(&b_coro, (void (*)(void *))b_fn, b_stack, sizeof b_stack);
    cron_coro_swap(&main_coro, &a_coro);
    int c = *(volatile int *)&counter;
    return c + n;
}
