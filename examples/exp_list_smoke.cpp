#include "meta_iostream.hpp"

#include <type_traits>
#include <utility>

using exp_utilities::exp_list;
using exp_utilities::exp_size;
using exp_utilities::literal_types::end_of_list;
using meta_objects::meta_produce_t;

namespace exp_list_tests {

using empty = exp_list<>;
using single = exp_list<int>;
using multiple = exp_list<int, double, char>;

static_assert(std::is_same_v<empty::pop_back, end_of_list>);
static_assert(std::is_same_v<single::pop_back, empty>);
static_assert(std::is_same_v<multiple::pop_back, exp_list<int, double>>);
static_assert(std::is_same_v<empty::push_back<int>::pop_back, empty>);
static_assert(std::is_same_v<single::push_back<double>::pop_back, single>);
static_assert(std::is_same_v<multiple::push_back<void>::pop_back, multiple>);
static_assert(std::is_same_v<multiple::pop_front, exp_list<double, char>>);
static_assert(std::is_same_v<multiple::front, int>);
static_assert(std::is_same_v<multiple::back, char>);
static_assert(std::is_same_v<multiple::at<1>, double>);
static_assert(multiple::length == 3 && exp_size<multiple> == 3);

struct incomplete;
using unusual = exp_list<void, incomplete, int&, const double, void()>;
static_assert(std::is_same_v<unusual::pop_back,
                             exp_list<void, incomplete, int&, const double>>);

using input = meta_invoke<meta_iostream::meta_istream<multiple>>;
using input_next = meta_invoke<input>;
using input_last = meta_invoke<input_next>;
using input_empty = meta_invoke<input_last>;
static_assert(std::is_same_v<input::type, multiple>);
static_assert(std::is_same_v<meta_produce_t<input>, int>);
static_assert(std::is_same_v<meta_produce_t<input_next>, double>);
static_assert(std::is_same_v<meta_produce_t<input_last>, char>);
static_assert(std::is_same_v<meta_produce_t<input_empty>, end_of_list>);
static_assert(std::is_same_v<meta_produce_t<
    meta_invoke<meta_iostream::meta_istream<empty>>>, end_of_list>);

// Preserve the existing reverse stream's prefix projection and pop_front advance.
using reverse = meta_invoke<meta_iostream::meta_ristream<multiple>>;
using reverse_next = meta_invoke<reverse>;
using reverse_last = meta_invoke<reverse_next>;
static_assert(std::is_same_v<reverse::type, multiple>);
static_assert(std::is_same_v<meta_produce_t<reverse>, exp_list<int, double>>);
static_assert(std::is_same_v<meta_produce_t<reverse_next>, exp_list<double>>);
static_assert(std::is_same_v<meta_produce_t<reverse_last>, empty>);

using output = meta_invoke<meta_iostream::meta_ostream<>, int>;
using output_next = meta_invoke<output, double>;
static_assert(std::is_same_v<meta_produce_t<output>, single>);
static_assert(std::is_same_v<meta_produce_t<output_next>, exp_list<int, double>>);

using forward = meta_invoke<
    meta_iostream::meta_forward_ostream<exp_list<int, double>>, char>;
using forward_next = meta_invoke<forward, long>;
using forward_overflow = meta_invoke<forward_next, void>;
static_assert(std::is_same_v<meta_produce_t<forward>, exp_list<exp_list<int, char>>>);
static_assert(std::is_same_v<meta_produce_t<forward_next>,
                             exp_list<exp_list<int, char>, exp_list<double, long>>>);
static_assert(std::is_same_v<meta_produce_t<forward_overflow>,
    exp_list<exp_list<int, char>, exp_list<double, long>, void>>);

template<class Sequence>
struct indexed_list;

template<std::size_t... I>
struct indexed_list<std::index_sequence<I...>> {
  using type = exp_list<std::integral_constant<std::size_t, I>...>;
};

template<std::size_t N>
constexpr bool prefix_matches() {
  using list = typename indexed_list<std::make_index_sequence<N>>::type;
  using prefix = typename indexed_list<std::make_index_sequence<N - 1>>::type;
  return std::is_same_v<typename list::pop_back, prefix>;
}

static_assert(prefix_matches<31>() && prefix_matches<32>());
static_assert(prefix_matches<33>() && prefix_matches<34>());
static_assert(prefix_matches<63>() && prefix_matches<64>());
static_assert(prefix_matches<65>() && prefix_matches<66>());

using deep = indexed_list<std::make_index_sequence<2048>>::type;
using deep_prefix = indexed_list<std::make_index_sequence<2047>>::type;
static_assert(exp_size<deep> == 2048);
static_assert(std::is_same_v<deep::pop_back, deep_prefix>);
static_assert(std::is_same_v<deep::push_back<void>::pop_back, deep>);
static_assert(deep::front::value == 0);
static_assert(deep::back::value == 2047);
static_assert(deep::at<1024>::value == 1024);
static_assert(exp_size<deep::pop_back> == 2047);

}  // namespace exp_list_tests

int main() {}
