#include "meta_ios.hpp"

#include <iostream>
#include <typeindex>
#include <vector>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_states_looper_details;

using stream_states_co = meta_ios::stream_states_co<>;

namespace stream_control_tests {
    struct select_second {
        template<class Window, class Flags, class Object>
        using apply = typename Flags::template select<
            meta_states::user_fn::index_of<Flags>() == 1, G_NOT_SKIP, 0>;
    };

    struct count_windows {
        template<class Window, class Flags, class Object>
        using apply = typename Flags::template inc<u64, 64>;
    };

    constexpr bool check_composition() {
        using source = meta_istream_list<char, double, int>;
        using selected = meta_states_for<meta_ios::stream_states_co<select_second>, meta_iterator, source>;
        unsigned calls = 0;
        selected::for_each([&]<class Stream>(Stream) {
            static_assert(std::is_same_v<meta_produce_t<typename Stream::to>, double>);
            ++calls;
        });
        return calls == 1;
    }
    static_assert(check_composition());

    using counted = meta_states_for<meta_ios::stream_states_co<count_windows>, meta_istream_list<int>>;
    static_assert(counted::type::states::get<u64, 64> == 7);
    static_assert(!counted::type::states::test<G_CONTINUE>);

    template<class Receiver, class Generator = meta_empty_o, class... Expected>
    bool check(const char* name, exp_list<Expected...>) {
        std::vector<std::type_index> actual;
        using loop = meta_states_for<stream_states_co, Receiver, Generator>;
        loop::for_each([&]<class Stream>(Stream) {
            actual.emplace_back(typeid(meta_produce_t<typename Stream::to>));
        });
        const std::vector<std::type_index> expected{std::type_index(typeid(Expected))...};
        const bool passed = actual == expected;
        std::cout << (passed ? "PASS: " : "FAIL: ") << name
                  << " (callbacks: " << actual.size() << ")\n";
        if (!passed) {
            std::cout << "  expected:";
            for (const auto& type : expected) std::cout << ' ' << type.name();
            std::cout << "\n  actual:  ";
            for (const auto& type : actual) std::cout << ' ' << type.name();
            std::cout << '\n';
        }
        return passed;
    }

    bool run() {
        using empty_stream = meta_istream_list<>;
        using single_stream = meta_istream_list<int>;
        using multiple_stream = meta_istream_list<char, double, int>;
        using initialized_stream = meta_invoke<multiple_stream>;
        using initialized_single = meta_invoke<single_stream>;
        static_assert(uninitialized_meta_object_t<multiple_stream>);
        static_assert(initialized_meta_object_t<initialized_stream>);
        static_assert(initialize_details::has_initializer<typename initialized_stream::function>);

        unsigned failed = 0;
        std::cout << "\nStream control boundary tests:\n";
        failed += !check<empty_stream>("empty receiver, no generator", exp_list<>{});
        failed += !check<meta_iterator, empty_stream>("empty generator", exp_list<>{});
        failed += !check<single_stream>("single receiver, no generator", exp_list<int>{});
        failed += !check<meta_iterator, single_stream>("single generator", exp_list<int>{});
        failed += !check<initialized_stream>("preinitialized receiver", exp_list<char, double, int>{});
        failed += !check<meta_iterator, initialized_stream>("preinitialized generator retaining initializer", exp_list<char, double, int>{});
        failed += !check<meta_iterator, initialized_single>("preinitialized single generator", exp_list<int>{});
        failed += !check<index_sequence_istream<0>>("zero-length index receiver", exp_list<>{});
        failed += !check<meta_aligned_iterator>("input-required receiver without generator", exp_list<>{});
        failed += !check<meta_aligned_iterator, empty_stream>("input-required receiver with empty generator", exp_list<>{});

        unsigned aligned_calls = 0;
        bool aligned_ok = true;
        meta_states_for<stream_states_co, meta_aligned_iterator, multiple_stream>
            ::for_each([&]<class Stream>(Stream) {
                using object = typename Stream::to_t;
                constexpr bool valid_type = std::is_same_v<typename object::type, char> ||
                    std::is_same_v<typename object::type, double> ||
                    std::is_same_v<typename object::type, int>;
                static_assert(valid_type);
                constexpr auto offset = object::value;
                aligned_ok = aligned_ok && (offset % alignof(typename object::type) == 0);
                ++aligned_calls;
            });
        aligned_ok = aligned_ok && aligned_calls == 3;
        failed += !aligned_ok;
        std::cout << (aligned_ok ? "PASS: " : "FAIL: ")
                  << "input-required receiver with generator (callbacks: " << aligned_calls << ")\n";
        std::cout << "Failed cases: " << failed << '\n';
        return failed == 0;
    }
}

int main() {
    return stream_control_tests::run() ? 0 : 1;
}