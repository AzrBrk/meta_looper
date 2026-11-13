// 可读类型名：编译期拿到的 string_view、以 '\0' 结尾的文本、以及编译期报错。
//
// 验证四件事：
//   1. type_name<T>() 在 constexpr 上下文可用（static_assert 直接比字符串）；
//   2. 引用 / cv / 数组 / 函数指针都切得对，不会把签名尾巴带出来；
//   3. META_TYPE_NAME_TEXT 给出以 '\0' 结尾的文本，能直接 printf("%s")；
//   4. demangle 能把 typeid(T).name() 的 mangled 名还原。
#include "meta_debug_type_name.hpp"

#include <cstdio>
#include <string>
#include <string_view>

#include "meta_utilities.hpp"

namespace {

struct sample {
  int value;
};

void print(std::string_view name) {
  std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
}

}  // namespace

int main() {
  // 1) 编译期可用：不依赖任何运行期状态
  static_assert(META_TYPE_NAME(int) == "int");
  static_assert(META_TYPE_NAME(const int&).find("int") != std::string_view::npos);
  static_assert(META_TYPE_NAME_TEXT(int)[0] == 'i');
  static_assert(META_TYPE_NAME_TEXT(int)[3] == '\0');

  // 2) 各类类型都切干净：不带 `]`、不带 `; std::string_view = ...`
  print(META_TYPE_NAME(int));
  print(META_TYPE_NAME(std::string));
  print(META_TYPE_NAME(exp_utilities::exp_list<int, double>));
  print(META_TYPE_NAME(int[3]));
  print(META_TYPE_NAME(const int&));
  print(META_TYPE_NAME(int (*)(long)));
  print(META_TYPE_NAME(sample));

  // 类型名里不允许混进签名剩余部分
  constexpr auto list_name = META_TYPE_NAME(exp_utilities::exp_list<int, double>());
  static_assert(list_name.find(']') == std::string_view::npos);
  static_assert(list_name.find(';') == std::string_view::npos);
  static_assert(list_name.find("exp_list<") != std::string_view::npos);

  // 3) 以 '\0' 结尾，直接喂 printf
  std::printf("text: %s\n", META_TYPE_NAME_TEXT(std::string_view));

  // 4) 对表达式取名字（去掉引用与 cv）
  const sample instance{};
  print(META_TYPE_NAME_OF(instance));
  std::printf("value type: %.*s\n", static_cast<int>(META_TYPE_NAME_OF(instance.value).size()),
              META_TYPE_NAME_OF(instance.value).data());

  // 5) 运行期 demangle：typeid 给的是 mangled，这里还原
  std::printf("demangled: %s\n", meta_debug::demangle(typeid(exp_utilities::exp_list<int, double>).name()).c_str());
  std::printf("demangled: %s\n", meta_debug::demangle(typeid(int (*)(long)).name()).c_str());

  // 6) 编译期把类型炸出来：取消下面这行的注释，编译器会在错误信息里打印
  //    `type_probe<...>` 的完整可读名，比 mangled 好读得多。
  // META_SHOW_TYPE(exp_utilities::exp_list<int, double>);

  std::puts("type_name_smoke: ok");
}
