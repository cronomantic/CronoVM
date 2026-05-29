/* conf_cpp_basic.cpp — conformance slice: core C++ codegen with NO runtime
 * dependency (no global ctors, no local statics, no exceptions, no RTTI, no
 * STL). Exercises the IR shapes C++ adds over C: virtual dispatch (vtables +
 * indirect call), inheritance/upcast, templates, operator overloading, member
 * functions (implicit `this`), references. Built -fno-exceptions -fno-rtti.
 *
 * extern "C" conf_main so the entry symbol is unmangled (the VM picks "main"
 * or the first function). Fixed-width types + int32 checksum -> differential
 * against the native build is exact. */
#include <stdint.h>

namespace {

struct Shape {
    int32_t base;
    Shape(int32_t b) : base(b) {}
    virtual int32_t area() const { return base; }
    virtual int32_t kind() const { return 0; }
    virtual ~Shape() {}
};

struct Square : Shape {
    Square(int32_t s) : Shape(s) {}
    int32_t area() const override { return base * base; }
    int32_t kind() const override { return 1; }
};

struct Box : Shape {
    int32_t h;
    Box(int32_t w, int32_t hh) : Shape(w), h(hh) {}
    int32_t area() const override { return base * h; }
    int32_t kind() const override { return 2; }
};

/* template — instantiated per type at compile time */
template <typename T>
static T tmax(T a, T b) { return a < b ? b : a; }

/* operator overloading + a small value type passed by value/reference */
struct Vec2 {
    int32_t x, y;
    Vec2 operator+(const Vec2 &o) const { return Vec2{ x + o.x, y + o.y }; }
    int32_t dot(const Vec2 &o) const { return x * o.x + y * o.y; }
};

/* polymorphic dispatch through a base reference */
static int32_t poly(const Shape &s) { return s.area() * 10 + s.kind(); }

} // namespace

extern "C" int conf_main(void) {
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(int32_t)(v)) * 16777619u; } while (0)

    for (int32_t i = 1; i <= 12; ++i) {
        Square sq(i);
        Box bx(i, i + 1);
        Shape sh(i);
        /* virtual dispatch via base pointer + reference */
        Shape *ps = (i & 1) ? (Shape *)&sq : (Shape *)&bx;
        MIX(ps->area());
        MIX(ps->kind());
        MIX(poly(sq));
        MIX(poly(bx));
        MIX(poly(sh));

        MIX(tmax<int32_t>(i, 100 - i));
        MIX(tmax<int16_t>((int16_t)i, (int16_t)(50 - i)));

        Vec2 a{ i, i * 2 }, b{ 3, -1 };
        Vec2 c = a + b;
        MIX(c.x); MIX(c.y);
        MIX(a.dot(b));
    }

    #undef MIX
    return (int)h;
}
