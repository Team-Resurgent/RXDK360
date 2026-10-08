/* std::string core operations. */
#include "rxdk_test.h"
#include <string>

int main() {
    std::string s;
    CHECK(s.empty(), "default construct is empty");

    s = "hello";
    CHECK_EQI(s.size(), 5, "size after assign");
    s += ", world";
    s.append("!");
    CHECK_STR(s.c_str(), "hello, world!", "append operator+ and append()");
    CHECK_EQI(s.size(), 13, "size after appends");

    CHECK_EQI((long)s.find("world"), 7, "find substring");
    CHECK(s.find("xyz") == std::string::npos, "find missing -> npos");

    std::string sub = s.substr(7, 5);
    CHECK_STR(sub.c_str(), "world", "substr");

    CHECK(std::string("abc") < std::string("abd"), "operator< lexicographic");
    CHECK(std::string("abc") == std::string("abc"), "operator== equal");

    s.replace(0, 5, "HELLO");
    CHECK_STR(s.c_str(), "HELLO, world!", "replace");

    std::string n = std::to_string(-1234);
    CHECK_STR(n.c_str(), "-1234", "to_string(int)");

    CHECK_EQI(std::stoi("2026"), 2026, "stoi");
    CHECK(std::stod("3.5") == 3.5, "stod");

    std::string big(100, 'x');
    CHECK_EQI(big.size(), 100, "fill construct 100");
    CHECK(big.find_first_not_of('x') == std::string::npos, "all x");

    /* Past the inline (SSO) capacity the string switches to its heap
       representation; the two overlay a flag bit, so every transition must keep
       the contents (the 360 once lost them when the flags did not overlap). */
    std::string grown("0123456789");
    grown += "abcdefghij";
    CHECK_STR(grown.c_str(), "0123456789abcdefghij", "grow past SSO via +=");
    CHECK_EQI(grown.size(), 20, "size after growing past SSO");
    CHECK(grown.capacity() >= 20, "capacity covers the heap string");
    grown.append(100, 'z');
    CHECK_EQI(grown.size(), 120, "append(n, c) on a heap string");
    CHECK(grown[119] == 'z' && grown[19] == 'j', "heap string contents after append");
    std::string copy = grown;
    CHECK(copy == grown && copy.data() != grown.data(), "copy of a heap string");
    grown.resize(5);
    grown.shrink_to_fit();
    CHECK_STR(grown.c_str(), "01234", "shrink a heap string back under SSO");
    std::string longlit("a string literal longer than the inline buffer");
    CHECK_EQI(longlit.size(), 46, "construct a heap string from a literal");
    CHECK_EQI((long)longlit.find("inline"), 33, "find in a heap string");

    CHECK_DONE("string");
    return 0;
}
