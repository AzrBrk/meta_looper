#include "meta_function_binder.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

using namespace meta_bind;

namespace binder_tests {

// ============================================================================
// 被绑定的函数 / 对象
// ============================================================================
int add(int a, int b) { return a + b; }
int answer() { return 42; }
void bump(int& x) { ++x; }
std::size_t text_size(std::string const& text) { return text.size(); }
int take_rvalue(std::string&& text) { return static_cast<int>(text.size()); }
int noex(int a) noexcept { return a + 1; }

struct widget {
  int base{10};
  int plus(int v) const { return base + v; }
  void set(int v) { base = v; }
};

struct alignas(32) over_aligned {
  int value;
};

struct counted {
  static inline int alive = 0;
  static inline int copies = 0;
  static inline int moves = 0;

  explicit counted(int v) : value{v} { ++alive; }
  counted(counted const& other) : value{other.value} {
    ++alive;
    ++copies;
  }
  counted(counted&& other) noexcept : value{other.value} {
    ++alive;
    ++moves;
  }
  counted& operator=(counted const&) = default;
  counted& operator=(counted&&) = default;
  ~counted() { --alive; }

  int value;
};

// 具名函数对象：拷贝/移动测试需要“同一个类型”的两个 binder。
struct counted_reader {
  int operator()(counted value) const { return value.value; }
};

template <class F>
bool throws_error(F&& call, error expected) {
  try {
    std::forward<F>(call)();
  } catch (exception const& e) {
    return e.code() == expected;
  } catch (...) {
    return false;
  }
  return false;
}

// 布局：参数栈就是 type_safe_aligned_range 的布局，两个 Alignment 都覆盖。
using add_binder = decltype(meta_bind::bind(add));
static_assert(add_binder::arg_count == 2);
static_assert(std::is_same_v<add_binder::return_type, int>);
// 2 个 int = 8 字节栈；+ 8 字节存活位图 + 2 字节游标 + 8 字节函数指针。
static_assert(add_binder::stack_bytes == 2 * sizeof(int));
static_assert(sizeof(add_binder) <= 32);

// 非 const 引用参数在栈里存一个指针；const 引用参数按值保存。
static_assert(std::is_same_v<decltype(meta_bind::bind(bump))::return_type, void>);
static_assert(decltype(meta_bind::bind(bump))::arg_count == 1);

bool run() {
  bool ok = true;

  {
    enum class slot : std::size_t { first, second };
    auto binder = meta_bind::bind(add);
    binder.set<slot::second>(5);
    ok = ok && binder.current_index() == 2 && binder.bound_count() == 1;
    ok = ok && throws_error([&] { (void)binder(); }, error::not_fully_bound);
    binder.set<slot::first>(3);
    ok = ok && binder.current_index() == 2 && binder() == 8;
    binder.reset_argc_stack();
    binder.set<1>(7);
    binder.set(slot::first, 4);
    ok = ok && binder() == 11;
    auto text = meta_bind::bind(text_size);
    text.set<0>("owned");
    ok = ok && text() == 5;
    int value = 8;
    auto reference = meta_bind::bind(bump);
    reference.set<0>(std::ref(value));
    reference();
    ok = ok && value == 9;
    std::cout << "static enum/integer slots: " << (ok ? "ok" : "FAIL") << '\n';
  }

  // ---- 基本绑定 / 调用 / 重复调用 ----
  {
    auto binder = meta_bind::bind(add);
    ok = ok && binder.argc_count() == 2 && binder.current_index() == 0;
    binder.bind(2);
    ok = ok && binder.current_index() == 1 && binder.bound_count() == 1 &&
         binder.is_bound(0) && !binder.is_bound(1);
    binder.bind(3);
    ok = ok && binder.bound_count() == 2;
    ok = ok && binder() == 5;
    ok = ok && binder.current_index() == 0;  // 调用后游标归零

    // operator()(args...) 重新写前两个槽位再调用
    ok = ok && binder(7, 8) == 15;
    ok = ok && binder(20, 22) == 42;

    // 运行期下标写回（受检代理）
    binder[0] = 10;
    binder[1] = 20;
    ok = ok && binder() == 30;

    // at<T> 给精确类型引用，可以就地修改
    binder.at<int>(0) += 1;
    ok = ok && binder() == 31;

    // 代理的 safe<T>() 与类型转换仍然可用：int 槽位能当 int 用。
    auto proxy = binder[0];
    ok = ok && proxy.safe<int>();
    ok = ok && static_cast<int&>(proxy) == binder.at<int>(0);

    // bind_a_lot / rebind_back
    binder.bind_a_lot(1, 2);
    binder.rebind_back(1);
    binder.bind(4);
    ok = ok && binder() == 5;

    std::cout << "basic: ok\n";
  }

  // ---- 零参数 ----
  {
    auto binder = meta_bind::bind(answer);
    static_assert(decltype(binder)::arg_count == 0);
    ok = ok && binder() == 42;
    ok = ok && binder() == 42;
    ok = ok && throws_error([&] { (void)binder.at<int>(0); }, error::index_exceed);
    std::cout << "zero-arg: ok\n";
  }

  {
    auto binder = meta_bind::bind([](int a, double b, int c) { return a + b + c; });
    binder.bind_a_lot(1, 2.5, 3);
    binder.at<double>(1) = 4.5;
    binder.at<int>(0) = 2;
    binder.at<int>(2) = 5;
    ok = ok && binder() == 11.5 && binder.at<double const>(1) == 4.5;
    using storage = type_safe_aligned::aligned_iterator_storage<int, double, int>;
    bool mismatch = false;
    bool overflow = false;
    try {
      (void)binder.at<double>(0);
    } catch (storage::exception const& e) {
      mismatch = e.code() == storage::error::type_not_safe;
    }
    try {
      (void)binder.at<double>(3);
    } catch (storage::exception const& e) {
      overflow = e.code() == storage::error::index_exceed;
    }
    ok = ok && mismatch && overflow;
  }

  // ---- 引用参数：写回调用者 ----
  {
    auto binder = meta_bind::bind(bump);
    int value = 5;
    binder.bind(std::ref(value));
    binder();
    ok = ok && value == 6;

    binder.bind(value);  // 左值也可以直接绑，栈里存的是它的地址
    binder();
    ok = ok && value == 7;

    // 右值不能绑到非 const 引用：这是编译期拒绝的，这里只验证运行期语义。
    std::cout << "reference arg: ok (x=" << value << ")\n";
  }

  // ---- const 引用参数：binder 自己保存一份值 ----
  {
    auto binder = meta_bind::bind(text_size);
    binder.bind(std::string{"hello"});
    ok = ok && binder() == 5;

    // 通过运行期下标改内容，再调用
    binder.at<std::string>(0) = "longer text";
    ok = ok && binder() == 11;
    std::cout << "const-ref arg owns value: ok\n";
  }

  // ---- 右值引用参数：调用时 move ----
  {
    auto binder = meta_bind::bind(take_rvalue);
    binder.bind(std::string{"abcd"});
    ok = ok && binder() == 4;
    std::cout << "rvalue-ref arg: ok\n";
  }

  // ---- 有捕获的 lambda / noexcept 函数 / std::function ----
  {
    int bias = 10;
    auto captured = meta_bind::bind([bias](int v) { return v + bias; });
    captured.bind(5);
    ok = ok && captured() == 15;

    auto ne = meta_bind::bind(noex);
    ne.bind(41);
    ok = ok && ne() == 42;

    auto ne_lambda = meta_bind::bind([](int value) noexcept { return value + 1; });
    ne_lambda.bind(1);
    ok = ok && ne_lambda() == 2;

    std::function<int(int, int)> erased = add;
    auto from_std_fn = meta_bind::bind(erased);
    from_std_fn.bind(3);
    from_std_fn.bind(4);
    ok = ok && from_std_fn() == 7;

    std::cout << "callable forms: ok\n";
  }

  // ---- 显式签名（泛型 lambda）----
  {
    auto generic = meta_bind::bind_as<int(int)>([](auto v) { return v * 3; });
    generic.bind(4);
    ok = ok && generic() == 12;

    auto generic_ref =
        meta_bind::bind_as<void(int&)>([](auto& v) { v += 10; });
    int value = 1;
    generic_ref.bind(std::ref(value));
    generic_ref();
    ok = ok && value == 11;

    std::cout << "bind_as: ok\n";
  }

  // ---- 成员函数 ----
  {
    widget object{};
    auto member = meta_bind::bind(object, &widget::plus);
    member.bind(5);
    ok = ok && member() == 15;

    auto setter = meta_bind::bind(object, &widget::set);
    setter.bind(99);
    setter();
    ok = ok && object.base == 99;

    widget const frozen{};
    auto const_member =
        meta_bind::bind(static_cast<widget const&>(frozen), &widget::plus);
    const_member.bind(1);
    ok = ok && const_member() == 11;

    std::cout << "member functions: ok\n";
  }

  // ---- 流输入：每个 >> 写下一个槽位 ----
  {
    auto binder = meta_bind::bind([](int a, double b) { return a + b; });
    std::istringstream input("3 4.5");
    input >> binder >> binder;
    ok = ok && binder() == 7.5;
    std::cout << "stream input: ok\n";
  }

  // ---- Alignment = 1（packed 参数栈）----
  {
    auto packed = meta_bind::bind<1>(
        [](char c, int i, double d) { return static_cast<int>(c) + i + d; });
    static_assert(decltype(packed)::stack_bytes == 1 + 4 + 8);
    packed.bind('a');
    packed.bind(2);
    packed.bind(3.5);
    ok = ok && packed() == 102.5;
    std::cout << "packed stack bytes=" << decltype(packed)::stack_bytes
              << " sizeof=" << sizeof(packed) << '\n';
  }

  // ---- 参数栈按最大类型对齐 ----
  {
    auto binder = meta_bind::bind([](over_aligned value) { return value.value; });
    static_assert(alignof(decltype(binder)) >= 32);
    binder.bind(over_aligned{7});
    ok = ok && binder() == 7;
    std::cout << "over-aligned slot: ok (sizeof=" << sizeof(binder) << ")\n";
  }

  // ---- 错误路径 ----
  {
    auto binder = meta_bind::bind(add);
    binder.bind(1);
    ok = ok && throws_error([&] { binder(); }, error::not_fully_bound);

    auto wrong = meta_bind::bind([](int a, std::string) { return a; });
    wrong.bind(1);
    // 第二个槽位是 string：double 只能赋值不能构造，未构造时按类型不匹配处理。
    ok = ok && throws_error([&] { wrong.bind(2.5); }, error::type_mismatch);

    binder.bind(2);
    ok = ok && throws_error([&] { binder.bind(3); }, error::argument_overflow);
    ok = ok && throws_error([&] { binder.rebind_back(3); }, error::index_exceed);
    binder.clear();
    ok = ok && binder.bound_count() == 0 && binder.current_index() == 0;

    std::cout << "error paths: ok\n";
  }

  // ---- 拷贝 / 移动：参数栈里的对象按值语义跟着搬 ----
  {
    counted::alive = counted::copies = counted::moves = 0;
    {
      // 用具名函数对象而不是 lambda：每个 lambda 表达式都是不同的闭包类型，
      // 用具名的类型才能让“同类型之间的赋值”可写。
      auto binder = meta_bind::bind(counted_reader{});
      binder.bind(counted{7});
      ok = ok && counted::alive == 1;
      ok = ok && binder() == 7 && counted::alive == 1;

      auto copied = binder;                    // 拷贝构造
      auto moved = std::move(copied);          // 移动构造
      ok = ok && moved() == 7 && binder() == 7;

      auto assigned = meta_bind::bind(counted_reader{});
      assigned = moved;                        // 拷贝赋值
      ok = ok && assigned() == 7;

      auto move_assigned = meta_bind::bind(counted_reader{});
      move_assigned = std::move(assigned);     // 移动赋值
      ok = ok && move_assigned() == 7;
    }
    ok = ok && counted::alive == 0 && counted::copies > 0 && counted::moves > 0;
    std::cout << "copy/move: ok (copies=" << counted::copies
              << " moves=" << counted::moves << ")\n";
  }

  // ---- 带捕获的 lambda：closure 的 operator= 是 deleted，binder 赋值要能绕开 ----
  {
    auto make = [](int bias) {
      return meta_bind::bind([bias](int value) { return value + bias; });
    };
    auto first = make(100);
    auto second = make(200);
    first.bind(1);
    second.bind(1);
    ok = ok && first() == 101 && second() == 201;

    first = second;  // 同一个闭包类型；走 reset_callable 的“重建”路径
    ok = ok && first() == 201;

    first = std::move(second);
    ok = ok && first() == 201;
    std::cout << "captured lambda assignment: ok\n";
  }

  return ok;
}

}  // namespace binder_tests

int main() {
  bool ok = binder_tests::run();
  std::cout << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? 0 : 1;
}
