#pragma once

// ============================================================================
// meta_debug_type_name —— 把类型名变成可读字符串
//
// 调试时最烦的两样：`typeid(T).name()` 给的 `_ZNSt7__cxx1112...`，以及模板报错里
// 那一屏 mangled 名字。这里不碰类型系统，只借编译器自己写进函数签名的明文：
//
//   template <class T> string_view type_signature() {
//     return __PRETTY_FUNCTION__;        // GCC/Clang
//     return __FUNCSIG__;                // MSVC
//   }
//
// 签名里必然带 `T = <名字>`，把这段切出来就是可读名。整个函数是 constexpr，
// 名字可以在 static_assert 里直接用，运行期零分配、零 `typeid`。
//
// 用法：
//   meta_debug::type_name<exp_list<int, double>>()      -> "exp_list<int, double>"
//   META_TYPE_NAME(exp_list<int, double>)              -> 同上（宏，单参数）
//   META_TYPE_NAME_OF(value)                           -> 对表达式去引用后的类型名
//   META_SHOW_TYPE(T);                                 -> 故意让编译失败，错误信息里带名字
//   META_TYPE_NAME_TEXT(T)                             -> 以 '\0' 结尾的 const char[]
//
// 注意：名字是**编译器看着办**的写法，不是标准规定的拼写。GCC 会写
// `std::__cxx11::basic_string<char>`，Clang(libc++) 写 `std::string`，
// MSVC 写 `class std::basic_string<...>`。要跨编译器比对字符串的地方别用它。
// ============================================================================

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(__GNUG__) || defined(__clang__)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace meta_debug {
namespace detail {

template <class T>
constexpr std::string_view type_signature() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  return __FUNCSIG__;
#elif defined(__GNUC__) || defined(__clang__)
  return __PRETTY_FUNCTION__;
#else
#error "meta_debug::type_name: no known signature macro for this compiler"
#endif
}

// 签名里的 `T = ...` 段：GCC 以 `; ` 收尾（后面还跟着别的模板参数），
// Clang 直接以 `]` 收尾。类型名里不可能出现 `;`，所以这刀是安全的。
template <class T>
constexpr std::string_view type_name_impl() noexcept {
  constexpr std::string_view signature = type_signature<T>();
#if defined(_MSC_VER) && !defined(__clang__)
  constexpr std::string_view open = "type_signature<";
  constexpr auto begin = signature.find(open) + open.size();
  constexpr auto end = signature.rfind(">(void)");
#else
  constexpr auto begin = signature.find("T = ") + 4;
  constexpr auto named = signature.find("; ", begin);
  constexpr auto end =
      named != std::string_view::npos ? named : signature.rfind(']');
#endif
  if constexpr (begin == std::string_view::npos ||
                end == std::string_view::npos || end <= begin) {
    return signature;
  } else {
    return signature.substr(begin, end - begin);
  }
}

}  // namespace detail

// 编译期可读类型名。返回值指向编译器生成的静态字符串，生命周期整个程序。
template <class T>
constexpr std::string_view type_name() noexcept {
  return detail::type_name_impl<T>();
}

// 对表达式取名字：去掉引用和 cv，`const std::string&` 也给出 `std::string`。
template <class T>
constexpr std::string_view type_name_of(T const&) noexcept {
  return type_name<std::remove_cvref_t<T>>();
}

// 以 '\0' 结尾的版本，专供 printf("%s") / 调试器 watch。
template <class T>
consteval auto make_type_name_text() noexcept {
  constexpr std::string_view name = type_name<T>();
  std::array<char, name.size() + 1> text{};
  for (std::size_t index = 0; index < name.size(); ++index) text[index] = name[index];
  return text;
}

template <class T>
inline constexpr auto type_name_text = make_type_name_text<T>();

// 故意实例化一个没有定义的模板：错误信息里会带上可读的实例化名字。
// GCC 会打印 `type_probe<exp_list<int, double> >`，Clang 打印
// `type_probe<exp_list<int, double>>`，两边都是 demangled 的。
template <class T>
struct type_probe;

template <class T>
inline constexpr std::size_t show_type = sizeof(type_probe<T>);

#if defined(__GNUG__) || defined(__clang__)
// 运行期 demangle：给 `typeid(T).name()`、backtrace 里的符号用。
// 走 abi::__cxa_demangle，会分配一块内存，返回 std::string。
inline std::string demangle(const char* mangled) {
  if (mangled == nullptr) return {};
  int status = 0;
  char* readable = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
  if (readable == nullptr || status != 0) {
    std::free(readable);
    return mangled;
  }
  std::string result = readable;
  std::free(readable);
  return result;
}

inline std::string demangle(std::string_view mangled) {
  return demangle(std::string(mangled).c_str());
}
#else
// MSVC 没有 __cxa_demangle；符号本来就是 MSVC 修饰名，需要 UnDecorateSymbolName，
// 这里不做假实现，直接原样返回。
inline std::string demangle(const char* mangled) {
  return mangled == nullptr ? std::string{} : std::string{mangled};
}
inline std::string demangle(std::string_view mangled) { return std::string{mangled}; }
#endif

}  // namespace meta_debug

// ---------------------------------------------------------------------------
// 宏：调用点尽量短，展开后只有一次函数调用/一个变量模板
// ---------------------------------------------------------------------------
#define META_TYPE_NAME(...) (::meta_debug::type_name<__VA_ARGS__>())
#define META_TYPE_NAME_OF(value) (::meta_debug::type_name_of(value))
#define META_TYPE_NAME_TEXT(...) (::meta_debug::type_name_text<__VA_ARGS__>.data())
#define META_SHOW_TYPE(...) ((void)::meta_debug::show_type<__VA_ARGS__>)
