/* Regression fixture for the "address taken but never internally called"
 * FUNCS-emission bug.
 *
 * `callback` has its address taken (passed to cvm_sys_register), but the
 * binary never CALLs or CALLRs it — the host is expected to invoke it later
 * via cvm_call. Before the fix, the translator only emitted a FUNCS section
 * when it saw a CALL/CALLR, so the function-pointer value handed to the host
 * indexed an absent table and cvm_call trapped with CVM_E_BAD_FUNCS.
 *
 * Driven by tests/test_fnptr_export.c: the host records the index passed to
 * cvm_sys_register, runs the entry, then cvm_call's the recorded index and
 * checks that it returns 42. */

extern void cvm_sys_register(int (*fn)(void));

/* Forward declaration so vm_main can reference callback's address while
 * still being the first *defined* function — the translator uses the first
 * definition as the entry point when there's no `main`. */
__attribute__((noinline))
int callback(void);

int vm_main(void) {
    cvm_sys_register(callback);
    return 0;
}

__attribute__((noinline))
int callback(void) {
    return 42;
}
