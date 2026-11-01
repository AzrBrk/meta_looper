#include "meta_utilities.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

using namespace exp_utilities;

// ---- 小列表 / 空列表 / 单元素：API 与结果类型不变 ----
static_assert(std::is_same_v<exp_list<int, double, char>::pop_back,
                             exp_list<int, double>>);
static_assert(std::is_same_v<exp_list<int>::pop_back, exp_list<>>);
static_assert(
    std::is_same_v<exp_list<>::pop_back, literal_types::end_of_list>);

// ---- void / 函数 / 引用元素：tuple 只做类型索引，type_identity 包一层 ----
using void_list = exp_list<void, int, double>;
static_assert(std::is_same_v<take<2, void_list>, exp_list<void, int>>);
static_assert(std::is_same_v<void_list::pop_back, exp_list<void, int>>);

using fn_list = exp_list<void(), int&, const int>;
static_assert(std::is_same_v<take<2, fn_list>, exp_list<void(), int&>>);
static_assert(std::is_same_v<fn_list::pop_back, exp_list<void(), int&>>);

// ---- take：取前 N 个元素，N 超过长度时取全部 ----
static_assert(
    std::is_same_v<take<2, exp_list<int, double, char>>, exp_list<int, double>>);
static_assert(std::is_same_v<take<0, exp_list<int, double, char>>, exp_list<>>);
static_assert(std::is_same_v<take<99, exp_list<int, double, char>>,
                             exp_list<int, double, char>>);
static_assert(std::is_same_v<take<2, std::tuple<int, double, char>>,
                             std::tuple<int, double>>);

// ---- 大列表：旧的逐元素累加在 ~900 层就撞深度；2000 项现在可以完整编译 ----
inline constexpr std::size_t pop_count = 2000;

template <std::size_t... Is>
auto make_numbers(std::index_sequence<Is...>)
    -> exp_list<std::integral_constant<std::size_t, Is>...>;

using numbers = decltype(make_numbers(std::make_index_sequence<pop_count>{}));
static_assert(numbers::length == pop_count);

using numbers_popped = typename numbers::pop_back;
static_assert(numbers_popped::length == pop_count - 1);
static_assert(
    std::is_same_v<typename numbers_popped::back,
                   std::integral_constant<std::size_t, pop_count - 2>>);

// pop_last 也是同一条 exp_select 路径；小列表上结果应与 pop_back 一致。
static_assert(std::is_same_v<pop_last<exp_list<int, double, char>>,
                             exp_list<int, double>>);

int main() {
  return 0;
}
