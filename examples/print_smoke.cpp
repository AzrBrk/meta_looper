#include "print.hpp"

#include <cassert>
#include <iostream>
#include <sstream>

struct rvalue_only {
    rvalue_only() = default;
    rvalue_only(const rvalue_only&) = delete;
};

std::ostream& operator<<(std::ostream& os, rvalue_only&&) {
    return os << "move";
}

int main() {
    std::ostringstream os;
    print<"hello #">(os, "world ");
    assert(os.str() == "hello world ");
    os.str("");
    print<"a#b#tail">(os, 12, 34);
    assert(os.str() == "a12b34tail");
    os.str("");
    print<"##">(os, "x", "y");
    assert(os.str() == "xy");
    os.str("");
    print<"plain text">(os);
    assert(os.str() == "plain text");
    os.str("");
    print<"">(os);
    assert(os.str().empty());
    os.str("");
    print<"#/#">(os, rvalue_only{}, 7);
    assert(os.str() == "move/7");
    os.str("");
    print<"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz#tail">(os, 42);
    assert(os.str() == "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz42tail");
    print<"hello #">(std::cout, "world ");
}
