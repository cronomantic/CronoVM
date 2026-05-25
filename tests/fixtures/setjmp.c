/* setjmp/longjmp end-to-end. setjmp returns 0; a nested (noinline) call longjmps
 * back with a value, and control resumes right after setjmp with that value and
 * the stack unwound (deep's frame discarded). Declared locally — the fixture
 * include path has no <setjmp.h>; the translator recognises the calls by name
 * and emits CVM_OP_SETJMP / CVM_OP_LONGJMP. State that must survive the longjmp
 * lives in a global, as the C contract requires.
 *
 *   vm_main(n): setjmp -> 0, deep(n+5) bumps counter then longjmps (n+5) back,
 *   second arrival has r==n+5 and counter==1 -> returns n+6.
 *   n=0 -> 6 ; n=10 -> 16. */

typedef int jmp_buf[4];
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

static jmp_buf jb;
static int     counter;

__attribute__((noinline))
static void deep(int v) {
    counter++;          /* proves the deeper frame actually ran */
    longjmp(jb, v);     /* never returns */
}

int vm_main(int n) {
    counter = 0;
    int r = setjmp(jb);
    if (r == 0) {
        deep(n + 5);    /* longjmps back with n+5 */
    }
    return r + counter; /* (n+5) + 1 */
}
