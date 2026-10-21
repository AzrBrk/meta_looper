#include "meta_utilities.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

using namespace exp_utilities;

// ---- 基本语义：= exp_select<From + Index> ----
using L = exp_list<int, double, char>;

static_assert(std::is_same_v<exp_select_from<0, 0, L>, int>);
static_assert(std::is_same_v<exp_select_from<0, 2, L>, char>);
static_assert(std::is_same_v<exp_select_from<1, 0, L>, double>);
static_assert(std::is_same_v<exp_select_from<1, 1, L>, char>);
static_assert(std::is_same_v<exp_select_from<2, 0, L>, char>);
static_assert(std::is_same_v<exp_select_from<0, 2, L>,
                             exp_select<2, L>>);

// void / 函数类型不经过 tuple 存储，也能正常选出
static_assert(std::is_same_v<exp_select_from<0, 0, exp_list<void, int>>, void>);
static_assert(std::is_same_v<exp_select_from<1, 0, exp_list<void, int>>, int>);
static_assert(
    std::is_same_v<exp_select_from<0, 0, exp_list<void()>>, void()>);

// ---- 越界：一律 no_exist_type，不硬错 ----
static_assert(
    std::is_same_v<exp_select_from<3, 0, L>, literal_types::no_exist_type>);
static_assert(
    std::is_same_v<exp_select_from<2, 1, L>, literal_types::no_exist_type>);
static_assert(
    std::is_same_v<exp_select_from<0, 3, L>, literal_types::no_exist_type>);
static_assert(
    std::is_same_v<exp_select_from<4, 0, L>, literal_types::no_exist_type>);
static_assert(std::is_same_v<exp_select_from<0, 0, exp_list<>>,
                             literal_types::no_exist_type>);

// ---- 非 type list：同样 no_exist_type ----
static_assert(
    std::is_same_v<exp_select_from<0, 0, int>, literal_types::no_exist_type>);
static_assert(
    std::is_same_v<exp_select_from<0, 0, void>, literal_types::no_exist_type>);

// ---- 巨大下标不溢出、不实例化 drop ----
inline constexpr std::size_t huge = std::numeric_limits<std::size_t>::max();
static_assert(std::is_same_v<exp_select_from<huge, 0, L>,
                             literal_types::no_exist_type>);
static_assert(std::is_same_v<exp_select_from<0, huge, L>,
                             literal_types::no_exist_type>);
static_assert(std::is_same_v<exp_select_from<huge, huge, L>,
                             literal_types::no_exist_type>);

// ---- 非 type list / 不完整类型 / 引用：同样 no_exist_type ----
struct incomplete;
static_assert(std::is_same_v<exp_select_from<0, 0, incomplete>,
                             literal_types::no_exist_type>);
static_assert(std::is_same_v<exp_select_from<0, 0, std::array<int, 3>>,
                             literal_types::no_exist_type>);
static_assert(
    std::is_same_v<exp_select_from<0, 0, int&>, literal_types::no_exist_type>);

// ---- 其他 type list 模板 ----
static_assert(std::is_same_v<exp_select_from<1, 1, std::tuple<int, double, char>>,
                             char>);

// ---- 2000 项：前缀 drop 复用，结果与 exp_select 一致 ----
template <std::size_t... Is>
auto make_numbers(std::index_sequence<Is...>)
    -> exp_list<std::integral_constant<std::size_t, Is>...>;

using Big = decltype(make_numbers(std::make_index_sequence<2000>{}));

static_assert(std::is_same_v<
              exp_select_from<1000, 999, Big>,
              std::integral_constant<std::size_t, 1999>>);
static_assert(std::is_same_v<exp_select_from<1000, 1000, Big>,
                             literal_types::no_exist_type>);
static_assert(std::is_same_v<
              exp_select_from<1999, 0, Big>,
              std::integral_constant<std::size_t, 1999>>);
static_assert(std::is_same_v<exp_select_from<1000, 999, Big>,
                             exp_select<1999, Big>>);

int main() {
  return 0;
}
