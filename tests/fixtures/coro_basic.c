/* Coroutine basic — covers CORO_SWAP's first-run (FRESH → FUNCS lookup) and
 * suspended-run (resume into saved PC) paths.
 *
 * The fixture mirrors the SDK's coro.h struct shape locally to avoid pulling
 * SDK headers (fixtures are freestanding clang -> .bc). The translator
 * recognises __cvm_coro_swap_raw by name and lowers to CVM_OP_CORO_SWAP.
 *
 * Test: vm_main(n) creates a worker that sets g_value = 42 then swaps back.
 * Returns g_value + n.  n=7 -> 49.
 */

#include <stdint.h>

typedef struct cron_coro {
    uint32_t _pc;
    uint32_t _sp;
    uint32_t _dest;
    uint32_t status;
    void   (*fn)(void *arg);    /* unused in this fixture (no trampoline) */
    void    *arg;
    void    *stack_lo;
    uint32_t stack_sz;
    struct cron_coro *resumer;
} cron_coro_t;

extern void __cvm_coro_swap_raw(cron_coro_t *from, cron_coro_t *to)
    __attribute__((__returns_twice__));

static cron_coro_t main_coro;
static cron_coro_t worker_coro;
static uint8_t     worker_stack[8192];
static int         g_value;

/* Worker entry — receives the coroutine itself in R0 (CORO_SWAP on FRESH
 * sets R[to.dest] = to, with dest = 0 = ABI arg0). The "who resumed me"
 * info lives in self->resumer, set by the cron_coro_swap wrapper before
 * the opcode (we mirror that here with a direct write). */
static void worker_fn(cron_coro_t *self) {
    g_value = 42;
    __cvm_coro_swap_raw(self, self->resumer);
    /* unreachable: when main runs us again it would have to re-CORO_SWAP us
     * but we already swapped out cleanly here. Falling off the end would
     * pop the trap sentinel and fault CVM_E_BAD_PC. */
}

int vm_main(int n) {
    /* Plant the trap sentinel at the new stack top so a stray RET faults. */
    uint8_t *top = worker_stack + sizeof worker_stack - 4;
    *(uint32_t *)top = 0xFFFFFFFEu;

    /* Manual cron_coro_init for the worker. _pc holds a function INDEX
     * because status=CORO_FRESH; the opcode does FUNCS[fn] on first swap. */
    worker_coro._pc    = (uint32_t)(uintptr_t)&worker_fn;
    worker_coro._sp    = (uint32_t)(uintptr_t)top;
    worker_coro._dest  = 0;          /* R0 = arg0 reg */
    worker_coro.status = 0;          /* CORO_FRESH */

    /* main_coro is freshly-zeroed; the first save into it populates pc/sp/
     * dest/status from the running cart context. No init needed.
     * Set the resumer link cart-side (the opcode doesn't touch field 32);
     * the SDK's cron_coro_swap wrapper would do this automatically. */
    worker_coro.resumer = &main_coro;
    __cvm_coro_swap_raw(&main_coro, &worker_coro);
    /* resumed here when worker swaps back. Volatile read of g_value forces
     * a real memory load (defeats clang's i1+select optimization that
     * would otherwise constant-fold across the call). */
    int v = *(volatile int *)&g_value;
    return v + n;
}
