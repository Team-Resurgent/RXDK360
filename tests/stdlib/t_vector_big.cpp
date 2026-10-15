/* Reproducer: std::vector growth with a large, non-power-of-two element size.
 *
 * AdvancedLighting's ATG scene parser builds a std::vector<XMLElementAttribute>
 * where sizeof(XMLElementAttribute) == 712 (WCHAR[100] + WCHAR[256]). On the
 * kit that vector's layout came back corrupt after a grow (__begin_/__end_ =
 * 0x80000000) and the next push_back stored into unmapped memory. The int
 * vector in t_containers never exercised a big / non-power-of-two element, so
 * this pins whether the grow/relocate path is size-dependent. */
#include "rxdk_test.h"
#include <vector>
#include <cstddef>

template <int N>
struct Blob {
    unsigned char b[N];
};

// Fill element i with a recognisable pattern, then verify every element after
// a series of growth-triggering push_backs.
template <int N>
static int run(const char* label) {
    std::vector<Blob<N>> v;
    const int count = 40;                 // forces several reallocations
    for (int i = 0; i < count; ++i) {
        Blob<N> e;
        for (int j = 0; j < N; ++j) e.b[j] = (unsigned char)((i * 7 + j) & 0xFF);
        v.push_back(e);                   // <-- fault site on the kit for N==712
    }
    if ((int)v.size() != count) { CHECK_EQI((int)v.size(), count, label); return 0; }
    int ok = 1;
    for (int i = 0; i < count && ok; ++i)
        for (int j = 0; j < N; ++j)
            if (v[i].b[j] != (unsigned char)((i * 7 + j) & 0xFF)) { ok = 0; break; }
    CHECK(ok, label);
    return ok;
}

int main() {
    CHECK_EQI((int)sizeof(Blob<712>), 712, "sizeof Blob<712>");
    run<712>("vector<712> grow (XMLElementAttribute size)");
    run<356>("vector<356> grow");
    run<100>("vector<100> grow");
    run<24>("vector<24> grow");
    run<64>("vector<64> grow (power of two)");
    run<712>("vector<712> grow (repeat)");
    CHECK_DONE("vector_big");
    return 0;
}
