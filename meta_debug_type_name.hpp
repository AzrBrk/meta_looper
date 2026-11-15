#pragma once

// ============================================================================
// meta_debug_type_name: readable type names at compile time.
//
// The compiler writes the demangled spelling of T into its own function
// signature (__PRETTY_FUNCTION__ / __FUNCSIG__). Slicing the `T = ...` part out
// of that signature yields a readable name without touching the type system:
//
//   template <class T> string_view type_signature() {
//     return __PRETTY_FUNCTION__;        // GCC / Clang
//     return __FUNCSIG__;                // MSVC
//   }
//
// The whole path is constexpr, so names are usable inside static_assert, and
// run time allocates nothing and never calls typeid.
//
// Usage:
//   meta_debug::type_name<exp_list<int, double>>()  -> "exp_list<int, double>"
//   META_TYPE_NAME(exp_list<int, double>)           -> same, single-argument macro
//   META_TYPE_NAME_OF(value)                        -> name of remove_cvref_t<decltype(value)>
//   META_SHOW_TYPE(T)                               -> fail the build, name in the diagnostic
//   META_TYPE_NAME_TEXT(T)                          -> NUL-terminated const char[]
//
// The spelling is implementation-defined, not standard: GCC prints
// `std::__cxx11::basic_string<char>`, Clang with libc++ prints `std::string`
// and MSVC prints `class std::basic_string<...>`. Never compare these strings
// across compilers; see README section "meta_debug_type_name.hpp".
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

// The `T = ...` field of the signature. GCC terminates it with `; ` (other
// template parameters follow) while Clang terminates it with `]`. A type name
// can never contain `;`, so this cut is unambiguous.
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

// Compile-time readable type name. The returned view points at a compiler-generated
// static string that lives for the whole program.
template <class T>
constexpr std::string_view type_name() noexcept {
  return detail::type_name_impl<T>();
}

// Name of an expression's type after remove_cvref: const std::string& yields
// std::string.
template <class T>
constexpr std::string_view type_name_of(T const&) noexcept {
  return type_name<std::remove_cvref_t<T>>();
}

// NUL-terminated variant for printf("%s") and debugger watch windows.
template <class T>
consteval auto make_type_name_text() noexcept {
  constexpr std::string_view name = type_name<T>();
  std::array<char, name.size() + 1> text{};
  for (std::size_t index = 0; index < name.size(); ++index) text[index] = name[index];
  return text;
}

template <class T>
inline constexpr auto type_name_text = make_type_name_text<T>();

// Intentionally instantiates an undefined template so the diagnostic shows the
// readable name.
// GCC prints `type_probe<exp_list<int, double> >`, Clang prints
// `type_probe<exp_list<int, double>>`; both are demangled.
template <class T>
struct type_probe;

template <class T>
inline constexpr std::size_t show_type = sizeof(type_probe<T>);

#if defined(__GNUG__) || defined(__clang__)
// Run-time demangling for `typeid(T).name()` and backtrace symbols.
// Uses abi::__cxa_demangle, which allocates once and returns a std::string.
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
// MSVC has no __cxa_demangle; its symbols need UnDecorateSymbolName instead.
// Rather than emulating it, this returns the input unchanged.
inline std::string demangle(const char* mangled) {
  return mangled == nullptr ? std::string{} : std::string{mangled};
}
inline std::string demangle(std::string_view mangled) { return std::string{mangled}; }
#endif

}  // namespace meta_debug

// ---------------------------------------------------------------------------
// Macros: short at the call site; each expands to one call or one variable
// template.
// ---------------------------------------------------------------------------
#define META_TYPE_NAME(...) (::meta_debug::type_name<__VA_ARGS__>())
#define META_TYPE_NAME_OF(value) (::meta_debug::type_name_of(value))
#define META_TYPE_NAME_TEXT(...) (::meta_debug::type_name_text<__VA_ARGS__>.data())
#define META_SHOW_TYPE(...) ((void)::meta_debug::show_type<__VA_ARGS__>)
