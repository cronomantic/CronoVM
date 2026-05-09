/* Static dispatch table: a const array of function pointers initialised
 * with function values. The translator must serialise each entry as the
 * function's FUNCS-table index, and the runtime LDW + CALLR resolves
 * the call. Exercises the path where function pointers live in DATA. */

typedef int (*op_t)(int, int);

__attribute__((noinline))
static int op_add(int a, int b) { return a + b; }

__attribute__((noinline))
static int op_sub(int a, int b) { return a - b; }

__attribute__((noinline))
static int op_mul(int a, int b) { return a * b; }

static const op_t ops[3] = { op_add, op_sub, op_mul };

int vm_main(int which, int a, int b) {
    return ops[which](a, b);
}
