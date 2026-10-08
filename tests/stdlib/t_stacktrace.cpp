/* <stacktrace> (C++23) -- implemented in the RXDK-360 llvm fork over the
   libunwind _Unwind_Backtrace already linked for exceptions. Symbolization is
   not available at runtime on the console (description()/source_file() return
   empty, which the standard permits); the captured addresses symbolize offline. */
#include "rxdk_test.h"
#include <stacktrace>
#include <string>

/* Each level must keep its own frame: optimized, `return level_x();` otherwise
   becomes a tail call and the frame never exists to be captured. */
#define LEVEL __attribute__((noinline, disable_tail_calls)) static
LEVEL std::stacktrace level_c() { return std::stacktrace::current(); }
LEVEL std::stacktrace level_b() { return level_c(); }
LEVEL std::stacktrace level_a() { return level_b(); }

int main() {
    std::stacktrace st = level_a();

    CHECK(!st.empty(), "current() captured a non-empty trace");
    CHECK(st.size() >= 4, "at least a/b/c + main frames captured");

    // frames carry real return addresses in the title's code region
    CHECK(st[0].native_handle() != 0, "top frame has a native handle");
    CHECK((bool)st[0], "top frame is non-empty (operator bool)");
    CHECK(st[0].native_handle() >= 0x82000000u, "address is in the loaded image");

    // no runtime symbolizer -> empty description/source, per the standard
    CHECK(st[0].description().empty(), "description empty (no runtime symbolizer)");
    CHECK_EQI((long)st[0].source_line(), 0, "source_line 0 (no runtime symbolizer)");

    // equality / entries iterate
    int walked = 0;
    for (auto&& f : st) { (void)f; ++walked; }
    CHECK_EQI(walked, (long)st.size(), "range-for visits every frame");

    // to_string renders frame index + address
    std::string s = std::to_string(st);
    CHECK(!s.empty() && s.find("0x") != std::string::npos, "to_string renders addresses");

    // depth limiting
    std::stacktrace shallow = std::stacktrace::current(0, 2);
    CHECK(shallow.size() <= 2, "current(skip,max) limits depth");

    CHECK_DONE("stacktrace");
    return 0;
}
