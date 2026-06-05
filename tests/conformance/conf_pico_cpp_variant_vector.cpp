/* conf_pico_cpp_variant_vector.cpp — conformance slice: a tagged-union value type
 * (à la Exult's Usecode_value) stored in std::vector<Self>, mutated through the
 * pointer-to-member-data "replace/replaceFrom" idiom.
 *
 * This reproduces the construct that miscompiled in the Exult bring-up (the
 * activate_eggs OOB / "null function pointer call", non-deterministic): a value
 * type that is an ANONYMOUS UNION of non-trivial members
 *   { long, std::string, std::vector<Self>, std::shared_ptr<...> }
 * whose lifetime is managed by hand (a type tag + placement-new construct() +
 * a destroy() switch), and whose assignment dispatches through
 *   replaceFrom(&Self::member, v2, tag)  ->  this->*ptr_to_data_member = v2.*ptr
 * i.e. POINTER-TO-DATA-MEMBER access onto the anonymous-union members. The
 * failing line in Exult is add_values()'s `arrayval[index] = val2` on a valid
 * 6-element array — replicated here verbatim in shape.
 *
 * None of the existing C++ fixtures (vector/map/string/memory/array/...) exercise
 * a union-of-non-trivial-members held in a vector + ptr-to-member-data, so the
 * miscompile slipped through. Differential: a value checksum over the array's
 * contents AFTER the mutations — so a wrong DATA result (not just a crash) is
 * caught.
 *
 * conf_pico* => the runner links picolibc.bc + pico_machine.c with picolibc's C
 * headers below the vendored libc++. The int32 checksum is platform-independent. */
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

class UV {
public:
    enum Tag {
        int_tag    = 0,
        string_tag = 1,
        array_tag  = 2,
        ptr_tag    = 3,
        sym_tag    = 4,
    };

    using Vec = std::vector<UV>;

private:
    Tag tag = int_tag;

    union {
        long                 intval;
        std::string          strval;
        Vec                  arrayval;
        std::shared_ptr<int> ptrval;
        void*                symval;
    };

    bool undefined = true;

    void destroy() noexcept {
        switch (tag) {
        case array_tag:
            arrayval.~Vec();
            break;
        case string_tag:
            using std::string;
            strval.~string();
            break;
        case ptr_tag:
            ptrval.~shared_ptr();
            break;
        default:
            break;
        }
    }

    template <typename T, typename... U>
    void construct(T& var, U&&... newval) {
        new (&var) T(std::forward<U>(newval)...);
    }

    template <typename T, typename U>
    void replace(T& var, U&& newval, Tag newtag, bool newundef = false) {
        if (tag == newtag) {
            var = std::forward<U>(newval);
        } else {
            destroy();
            tag = newtag;
            construct(var, std::forward<U>(newval));
        }
        undefined = newundef;
    }

    template <typename T, typename U>
    void replaceFrom(T var, U&& newval, Tag newtag) {
        const bool newundef = newval.undefined;
        replace(this->*var, std::forward<U>(newval).*var, newtag, newundef);
    }

    template <typename T>
    void copy_internal(T&& v2) {
        const Tag newtag = v2.tag;
        switch (newtag) {
        case int_tag:
            replaceFrom(&UV::intval, std::forward<T>(v2), newtag);
            break;
        case ptr_tag:
            replaceFrom(&UV::ptrval, std::forward<T>(v2), newtag);
            break;
        case string_tag:
            replaceFrom(&UV::strval, std::forward<T>(v2), newtag);
            break;
        case array_tag:
            replaceFrom(&UV::arrayval, std::forward<T>(v2), newtag);
            break;
        case sym_tag:
            replaceFrom(&UV::symval, std::forward<T>(v2), newtag);
            break;
        }
    }

public:
    UV() : intval(0) {}

    explicit UV(int v) : intval(v), undefined(false) {}

    explicit UV(const std::string& s) : tag(string_tag), strval(s), undefined(false) {}

    // Create array with 1st element (mirrors Usecode_value(size, elem0)).
    UV(int size, UV* elem0) : tag(array_tag), arrayval(size), undefined(false) {
        if (elem0) {
            arrayval[0] = *elem0;
        }
    }

    ~UV() {
        destroy();
    }

    UV& operator=(const UV& v2) {
        if (&v2 != this) {
            copy_internal(v2);
        }
        return *this;
    }

    UV& operator=(UV&& v2) noexcept {
        copy_internal(std::move(v2));
        return *this;
    }

    UV& operator=(int v) noexcept {
        replace(intval, v, int_tag);
        return *this;
    }

    UV(const UV& v2) {
        *this = v2;
    }

    UV(UV&& v2) noexcept {
        *this = std::move(v2);
    }

    Tag get_tag() const {
        return tag;
    }

    bool is_array() const {
        return tag == array_tag;
    }

    size_t get_array_size() const {
        return (tag == array_tag) ? arrayval.size() : 0;
    }

    long get_int_value() const {
        return (tag == int_tag) ? intval : 0;
    }

    size_t get_str_len() const {
        return (tag == string_tag) ? strval.size() : 0;
    }

    UV& operator[](int i) {
        return arrayval[i];
    }

    const UV& operator[](int i) const {
        return arrayval[i];
    }

    // The exact failing routine: add value at index, or push if past the end.
    int add_values(int index, UV& val2) {
        const int size = (int)get_array_size();
        if (!val2.is_array()) {
            if (index >= size) {
                arrayval.push_back(val2);
            } else {
                arrayval[index] = val2;    // <- the miscompiled line in Exult
            }
            return 1;
        }
        const int size2 = (int)val2.get_array_size();
        for (int i = 0; i < size2; ++i) {
            const int dst = index + i;
            if (dst >= (int)get_array_size()) {
                arrayval.push_back(val2[i]);
            } else {
                arrayval[dst] = val2[i];
            }
        }
        return size2;
    }
};

/* Fold the array's contents into a stable int32 checksum (data, not pointers). */
static int32_t checksum(const UV& a) {
    int32_t acc = (int32_t)a.get_array_size() * 7;
    for (size_t i = 0; i < a.get_array_size(); ++i) {
        const UV& e = a[i];
        acc = acc * 31 + (int32_t)e.get_tag();
        acc = acc * 31 + (int32_t)e.get_int_value();
        acc = acc * 31 + (int32_t)e.get_str_len();
        acc = acc * 31 + (int32_t)e.get_array_size();
    }
    return acc;
}

extern "C" int conf_main() {
    int32_t acc = 0;

    /* Build a 6-element int array (default elements: int 0), exactly like
     * Usecode_value(size, elem0) does — arrayval(6) default-constructs 6 ints. */
    volatile int seed = 1000;
    UV first((int)seed);
    UV arr(6, &first);
    acc += (int32_t)arr.get_array_size();    /* +6 */

    /* add_values(index, int) over a VALID array -> arrayval[index] = val2.
     * This is the precise Exult path. Non-foldable seeds so the ops run. */
    for (int i = 0; i < 6; ++i) {
        volatile int s = (i + 1) * 11;
        UV v((int)s);
        arr.add_values(i, v);
    }
    acc += checksum(arr);

    /* Transition members: drop a string into an element (int->string forces a
     * destroy()+construct via the != branch of replace), then put an int back
     * (string->int forces the other destroy()). Exercises replaceFrom on the
     * string member + the destroy() switch. */
    {
        UV s(std::string("cronopio-variant"));
        arr.add_values(2, s);            /* element 2 becomes a string */
        acc += (int32_t)arr[2].get_str_len();   /* +16 */
        UV back((int)777);
        arr.add_values(2, back);         /* string -> int, destroy() the string */
        acc += (int32_t)arr[2].get_int_value();  /* +777 */
    }

    /* A genuine nested array element (vector<Self>) assigned DIRECTLY into the
     * parent (not via add_values, which would spread an array's elements),
     * stressing vector-of-Self copy through the array_tag replaceFrom path. */
    {
        UV inner_elem((int)5);
        UV inner(3, &inner_elem);        /* [5,0,0] */
        UV intval4((int)4);
        inner.add_values(1, intval4);    /* [5,4,0] */
        arr[1] = inner;                  /* element 1 becomes a nested array (copy of [5,4,0]) */
        acc = acc * 31 + (int32_t)arr[1].get_array_size();  /* nested size 3 */
        acc = acc * 31 + (int32_t)arr[1][1].get_int_value();/* 4 */
    }

    /* Grow past the end (push_back path of add_values). */
    {
        UV tail((int)321);
        arr.add_values(99, tail);
        acc += (int32_t)arr.get_array_size();   /* now 7 */
        acc += (int32_t)arr[6].get_int_value(); /* +321 */
    }

    acc += checksum(arr);
    return acc;
}
