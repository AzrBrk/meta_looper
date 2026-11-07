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

template<exp_utilities::static_str Format>
consteval bool callbacks_are_initialized() {
    using loop = meta_states::run<
        meta_ios::stream_states_co<print_detail::condition>,
        meta_iostream::meta_ostream<>,
        meta_iostream::meta_istream<exp_utilities::str_to_list<Format>>>;
    unsigned calls = 0;
    loop::for_each([&](auto stream) {
        using object = typename decltype(stream)::to;
        static_assert(meta_objects::initialized_meta_object_t<object>);
        using received = decltype(stream.object());
        static_assert(std::is_same_v<typename received::back, exp_utilities::exp_char<'#'>> ||
                      std::is_same_v<typename received::back, exp_utilities::exp_char<'\0'>>);
        using fragment = typename received::pop_back;
        using terminated = typename fragment::template push_back<exp_utilities::exp_char<'\0'>>;
        (void)exp_utilities::to_exp_char_t<terminated>{};
        ++calls;
    });
    unsigned expected = 1;
    for (char c : Format.str) expected += c == '#';
    return calls == expected;
}

static_assert(callbacks_are_initialized<"">());
static_assert(callbacks_are_initialized<"##">());
static_assert(callbacks_are_initialized<"#/#">());
static_assert(callbacks_are_initialized<"hello #">());
static_assert(callbacks_are_initialized<"a#b#tail">());

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
