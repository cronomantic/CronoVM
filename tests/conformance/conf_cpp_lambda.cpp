/* conf_cpp_lambda.cpp — conformance slice: lambdas / closures. Non-capturing
 * (convertible to a function pointer), by-value capture, by-reference capture
 * (the closure holds a pointer), and a generic (templated operator()) lambda.
 * A closure is just a compiler-generated struct with an operator(); calling one
 * is an ordinary member call. Also exercises a lambda passed through a function
 * pointer. Differential; -fno-exceptions -fno-rtti. */
#include <stdint.h>

namespace {
typedef int32_t (*Fn)(int32_t);

static int32_t apply(Fn f, int32_t x) { return f(x); }

template <typename F>
static int32_t apply_t(F f, int32_t x) { return f(x); }
} // namespace

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int32_t i = 0; i < 12; ++i) {
        /* non-capturing lambda -> function pointer */
        Fn fp = [](int32_t x) { return x * x - 1; };
        MIX(apply(fp, i));

        /* by-value capture */
        int32_t cap = i * 7;
        auto byval = [cap](int32_t x) { return x + cap; };
        MIX(byval(i));
        MIX(apply_t(byval, 100));

        /* by-reference capture (closure holds &acc; mutates through it) */
        int32_t acc = 0;
        auto bump = [&acc](int32_t x) { acc += x; return acc; };
        MIX(bump(i));
        MIX(bump(i + 1));
        MIX(acc);                       /* reflects both mutations */

        /* generic lambda (templated operator()) */
        auto gen = [](auto a, auto b) { return a * b; };
        MIX(gen(i, (int32_t)3));
        MIX((int32_t)gen((int16_t)i, (int16_t)5));
    }

    #undef MIX
    return (int)h;
}
