#include "meta_stream64.hpp"
#include "meta_looper64.hpp"
#include "meta_iostream.hpp"

#include <string>
#include <type_traits>
#include <utility>

using namespace meta_core64;
using namespace meta_looper64;
using namespace meta_iostream;
using namespace meta_stream64;
using namespace meta_objects;

// args_at<I>(args...) —— 返回第 I 个实参，引用与值类别原样保留
template <u64 I, class... Tys>
decltype(auto) args_at(Tys&&... args) {
  using cond = stream_cond64<exactly_at<I>>;
  using gen = index_sequence_istream<sizeof...(Tys)>;
  return looper<cond, meta_empty_o, gen>::for_each_forward(
      [](auto, auto&& value) -> decltype(auto) {
        return std::forward<decltype(value)>(value);
      },
      std::forward<Tys>(args)...);
}

// 任意下标，不同类型
static_assert(std::is_same_v<decltype(args_at<1>(1, 2.5, 'k')), double&&>);
static_assert(std::is_same_v<decltype(args_at<2>(1, 2.5, 'k')), char&&>);

// 左值 / const 左值 / 右值
consteval bool value_categories() {
  std::string s = "ggg";
  const std::string cs = "ccc";
  static_assert(std::is_same_v<decltype(args_at<1>(1, s, 'k')), std::string&>);
  static_assert(std::is_same_v<decltype(args_at<1>(1, cs, 'k')), const std::string&>);
  static_assert(std::is_same_v<decltype(args_at<1>(1, std::string("ggg"), 'k')),
                               std::string&&>);
  return true;
}
static_assert(value_categories());

int main() { return 0; }
