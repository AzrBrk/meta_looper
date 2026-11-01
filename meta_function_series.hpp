#pragma once

// ============================================================================
// meta_function_series.hpp —— 把多个 function_binder 串成一条函数链
//
// 对标 select_type 的 exp_function_series，但：
//
//   1) 每段的参数栈各自是一块内联的 type_safe_aligned_range（见
//      meta_function_binder.hpp），整条 series 只由
//
//        std::tuple<Binders...>            —— 每段一个 binder
//        std::byte[field_size<...>]        —— 每段返回值的对齐缓冲
//        series_index / argc_index         —— 两个 16 位游标
//
//      组成，没有链表、shared_ptr 或逐节点堆分配。
//
//   2) 返回值**不再放在 std::tuple 里**：所有段的返回值按
//      type_safe_aligned_range 的布局排在一条对齐缓冲里（和参数栈用同一套
//      field_size / aligned_offset_stream）。于是：
//
//        auto [m, w, ok] = series._return();     // 结构化绑定
//        series._return().get<1>()               // 编译期下标，精确类型 T&
//        series._return()[1]                     // 运行期下标，受检代理
//        series._return<1>()                     // 同上，成员模板写法
//        series.final_return()                   // 最后一段的返回值
//
//      void 段没有值，槽位按 void* 存（nullptr），和旧库 _return() 的占位一致。
//
// 3) 执行有两条路：
//
//      _continue() / _bind_continue()   编译期展开，一段接一段直接调用（快）
//      drive()                          用底层 looper 状态机驱动：每段是一轮，
//                                       由 driver 分块推进，每轮回调把该段的
//                                       返回值写进对齐缓冲；遇到很长的链也不会
//                                       加深模板递归。
//      run_loop(n, fn)                  重复跑 n 次“reset + drive”，每次把
//                                       本次的返回 range 交给 fn。
//
// 典型用法：
//
//   auto s = meta_bind::link_f(
//       [](int a, double b) { return b - a; },      // 段 0
//       [](double c) { return std::to_string(c); }, // 段 1
//       []() { std::cout << "end\n"; });            // 段 2
//
//   meta_bind::series_bind(s, 1, 1.33, 0.33);       // 按段顺序灌参数
//   s._continue();                                  // 依次执行到底
//   auto [first, second, v] = s._return();          // void -> void*(nullptr)
//
// 或者让上一段的返回值自动进入下一段：
//
//   auto s = meta_bind::link_f([](int a) { return a * 2; },
//                              [](int v) { return v + 1; });
//   s.bind(21);          // 只给段 0 的参数
//   s._bind_continue();  // 段 1 自动收到 42
//   s.final_return();    // 43
//
// 语义要点：
//   - 每段自己的游标与参数栈独立；series_bind 的扁平分配按“所有段槽位顺序”
//     进行，可以从 argc_at(n) 指定的槽位继续；
//   - _continue 从当前 series_index 跑到最后，_stop_at(i) 跑到第 i 段（含）；
//   - _bind_continue / drive 会把上一段返回值 append 到下一段当前的绑定游标
//     （下一段没有参数或上一段返回 void 时跳过）；
//   - 返回值缓冲在 series 构造时按默认值构造，执行时写回；拷贝/移动随 series
//     走（所以 series 仍然是普通值类型）；
//   - 任何越界 / 类型不匹配 / 参数未绑满都抛 meta_bind::exception。
// ============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "meta_function_binder.hpp"
#include "meta_states.hpp"

namespace meta_bind {

namespace detail {

// void 没有值：槽位用 void*（nullptr）占位，和旧库 _return() 的占位一致。
template <class R>
struct return_storage {
  using type = R;
};
template <>
struct return_storage<void> {
  using type = void*;
};

template <class Binder>
using return_slot_of = typename return_storage<typename Binder::return_type>::type;

// looper 条件：位置（已完成的轮数）到 stage_count 就关总闸；
// 其余窗口原样放行。每次 drive() 是一个独立的 looper 运行。
template <std::size_t Stages>
struct series_round_executor {
  template <class Stage, class Flags, class Object>
  struct apply {
    static constexpr auto value = [] {
      if constexpr (Stage::value == meta_states::pre_gen_exec) {
        if (meta_states::user_fn::index_of<Flags>() >= Stages) {
          return Flags::template opDeact<meta_states::G_CONTINUE>::value;
        }
      }
      return Flags::value;
    }();
  };
};

}  // namespace detail

template <class... Binders>
class function_series {
  static_assert(sizeof...(Binders) > 0,
                "function_series requires at least one binder");

 public:
  static constexpr std::size_t stage_count = sizeof...(Binders);
  static constexpr std::size_t return_count = stage_count;
  static constexpr std::size_t total_args = (Binders::arg_count + ... + 0);

 private:
  using stages_type = std::tuple<Binders...>;

  // 返回值槽位：与参数栈同一套 aligned_range 布局（Alignment = 0）。
  using returns_list =
      exp_list<detail::return_slot_of<Binders>...>;
  using returns_layout = detail::aligned_slot_layout<0, returns_list>;
  using returns_range = typename returns_layout::range_type;

  template <std::size_t I>
  using return_slot = typename returns_layout::template slot<I>;
  template <std::size_t I>
  using return_cursor = typename returns_layout::template cursor<I>;

  static_assert(
      ((std::is_void_v<typename Binders::return_type> ||
        std::is_default_constructible_v<typename Binders::return_type>)&&...),
      "function_series: every non-void return type must be default "
      "constructible (reference returns are not supported)");

  static_assert(
      ((std::is_void_v<typename Binders::return_type> ||
        std::is_constructible_v<detail::return_slot_of<Binders>,
                                detail::return_slot_of<Binders>&&>)&&...),
      "function_series: return values must be move- or copy-constructible");

  static constexpr bool stages_copyable =
      (std::is_copy_constructible_v<Binders> && ...);
  static constexpr bool stages_movable =
      (std::is_move_constructible_v<Binders> && ...);
  static constexpr bool returns_copyable =
      (std::is_copy_constructible_v<detail::return_slot_of<Binders>> && ...);
  static constexpr bool returns_movable =
      (std::is_move_constructible_v<detail::return_slot_of<Binders>> && ...);

  // 第 I 段的参数个数 / 它的第一个槽位在所有段槽位里的偏移。
  template <std::size_t I>
  static constexpr std::size_t stage_arity() {
    return std::tuple_element_t<I, stages_type>::arg_count;
  }

  template <std::size_t I, std::size_t J = 0, std::size_t Acc = 0>
  static constexpr std::size_t stage_start_impl() {
    if constexpr (J == I) {
      return Acc;
    } else {
      return stage_start_impl<I, J + 1, Acc + stage_arity<J>()>();
    }
  }

  template <std::size_t I>
  static constexpr std::size_t stage_start() {
    return stage_start_impl<I>();
  }

  template <class Arg>
  static constexpr bool any_stage_accepts() {
    return (Binders::template accepts_all<Arg>() || ...);
  }

 public:
  // ---- 构造 / 析构 ----
  explicit function_series(Binders... binders)
      : stages_(std::move(binders)...) {
    construct_returns(std::make_index_sequence<stage_count>{});
  }

  function_series(function_series const& other)
    requires(stages_copyable && returns_copyable)
      : stages_(other.stages_),
        series_index_(other.series_index_),
        argc_index_(other.argc_index_) {
    copy_returns(other, std::make_index_sequence<stage_count>{});
  }

  function_series(function_series&& other)
    requires(stages_movable && returns_movable)
      : stages_(std::move(other.stages_)),
        series_index_(other.series_index_),
        argc_index_(other.argc_index_) {
    move_returns(other, std::make_index_sequence<stage_count>{});
  }

  function_series& operator=(function_series const& other)
    requires(stages_copyable && returns_copyable) {
    if (this == &other) return *this;
    stages_ = other.stages_;
    series_index_ = other.series_index_;
    argc_index_ = other.argc_index_;
    destroy_returns();
    copy_returns(other, std::make_index_sequence<stage_count>{});
    return *this;
  }

  function_series& operator=(function_series&& other)
    requires(stages_movable && returns_movable) {
    if (this == &other) return *this;
    stages_ = std::move(other.stages_);
    series_index_ = other.series_index_;
    argc_index_ = other.argc_index_;
    destroy_returns();
    move_returns(other, std::make_index_sequence<stage_count>{});
    return *this;
  }

  ~function_series() { destroy_returns(); }

  // ---- 访问 ----
  stages_type& stages() noexcept { return stages_; }
  stages_type const& stages() const noexcept { return stages_; }

  template <std::size_t I>
  auto& stage() noexcept {
    static_assert(I < stage_count, "function_series::stage: index out of range");
    return std::get<I>(stages_);
  }

  template <std::size_t I>
  auto const& stage() const noexcept {
    static_assert(I < stage_count, "function_series::stage: index out of range");
    return std::get<I>(stages_);
  }

  static constexpr std::size_t size() noexcept { return stage_count; }
  std::size_t series_index() const noexcept { return series_index_; }
  std::size_t argc_index() const noexcept { return argc_index_; }

  // 返回值视图：type_safe_aligned_range，编译期 / 运行期下标都可用。
  returns_range _return() noexcept {
    return returns_range{returns_storage_.data()};
  }

  // range 是视图（constness 不向元素传播，与 std::span 一致），所以 const
  // series 上返回的仍是同一个可写视图。
  returns_range _return() const noexcept {
    return returns_range{const_cast<std::byte*>(returns_storage_.data())};
  }

  returns_range returns() noexcept { return _return(); }
  returns_range returns() const noexcept { return _return(); }

  // 编译期下标：精确类型的返回值引用（零检查）。
  template <std::size_t I>
  auto& _return() noexcept {
    static_assert(I < stage_count, "function_series::_return<I>: out of range");
    return return_at<I>();
  }

  template <std::size_t I>
  auto const& _return() const noexcept {
    static_assert(I < stage_count, "function_series::_return<I>: out of range");
    return return_at<I>();
  }

  decltype(auto) final_return() noexcept {
    return return_at<stage_count - 1>();
  }

  decltype(auto) final_return() const noexcept {
    return return_at<stage_count - 1>();
  }

  // 选择当前段 / 扁平参数游标（与旧库同名）。
  function_series& operator[](std::size_t index) {
    if (index >= stage_count) throw exception{error::index_exceed};
    series_index_ = static_cast<std::uint16_t>(index);
    return *this;
  }

  function_series& argc_at(std::size_t index) {
    if (index > total_args) throw exception{error::argument_overflow};
    argc_index_ = static_cast<std::uint16_t>(index);
    return *this;
  }

  // ---- 绑定到“当前段” ----
  template <class... Args>
  void bind(Args&&... args) {
    if constexpr (sizeof...(Args) > 0) {
      static_assert((any_stage_accepts<Args>() && ...),
                    "function_series::bind: no stage can accept these arguments");
    }
    if (series_index_ >= stage_count) throw exception{error::index_exceed};
    bind_stage<0>(series_index_, std::forward<Args>(args)...);
  }

  // ---- 扁平绑定：按段顺序把所有段的槽位排成一条参数流 ----
  template <class... Args>
  std::size_t bind_flat(Args&&... args) {
    if constexpr (sizeof...(Args) > 0) {
      static_assert((any_stage_accepts<Args>() && ...),
                    "function_series::bind_flat: no stage can accept these "
                    "arguments");
    }
    std::size_t position = argc_index_;
    (bind_flat_one(position, std::forward<Args>(args)), ...);
    argc_index_ = static_cast<std::uint16_t>(position);
    return position;
  }

  // ---- 执行 ----
  void execute() {
    if (series_index_ >= stage_count) throw exception{error::index_exceed};
    execute_stage_dispatch<0>(series_index_);
    ++series_index_;
  }

  void _continue() { continue_stage<0>(); }

  void _stop_at(std::size_t index) {
    while (series_index_ <= index) {
      if (series_index_ >= stage_count) break;
      execute();
    }
  }

  void _bind_continue() { bind_continue_stage<0>(); }

  // 用底层 looper 状态机逐段执行：每段一轮，driver 负责推进/分块；
  // 每轮回调按本轮编号执行对应段（编号是 constexpr，所以没有运行期分派）。
  void drive() {
    if (series_index_ >= stage_count) return;
    using condition = meta_states::make_condition<
        detail::series_round_executor<stage_count>>;
    meta_states::run<condition, meta_states::meta_empty_o>::for_each(
        [this](auto stage) {
          constexpr std::size_t index = stage.index();
          if (index >= series_index_) advance_stage<index>();
        });
  }

  // 重复执行 count 次：每次 reset + drive，把本次的返回 range 交给回调。
  // 回调里读到的 range 指向 series 的返回缓冲，下一次 run_loop 迭代会覆盖。
  template <class Fn>
  void run_loop(std::size_t count, Fn&& on_returns) {
    for (std::size_t i = 0; i < count; ++i) {
      reset();
      drive();
      on_returns(_return());
    }
  }

  // 把上一段的返回值绑进当前段（旧库同名入口）。
  void bind_last_return() {
    if (series_index_ == 0 || series_index_ >= stage_count) {
      throw exception{error::index_exceed};
    }
    bind_last_dispatch<0>();
  }

  // 清空所有段的绑定游标与扁平游标（不清返回值）。
  void clear_all_argc_idx() {
    std::apply(
        [](auto&... binder) { (binder.reset_argc_stack(), ...); }, stages_);
    argc_index_ = 0;
  }

  // 重新从第 0 段开始：series 游标、扁平游标、各段绑定游标全部归零。
  // 已经绑过的参数栈保留（再次 bind 会覆盖），所以“同一组参数再跑一遍”
  // 只需要 reset() + _continue()（或 drive()）。
  void reset() {
    series_index_ = 0;
    clear_all_argc_idx();
  }

 private:
  template <std::size_t I>
  std::byte* return_address() noexcept {
    return return_cursor<I>{}.address_of(returns_storage_.data());
  }

  template <std::size_t I>
  auto& return_at() noexcept {
    return return_cursor<I>{}.get(returns_storage_.data());
  }

  template <std::size_t I>
  auto const& return_at() const noexcept {
    return return_cursor<I>{}.get(returns_storage_.data());
  }

  template <std::size_t... I>
  void construct_returns(std::index_sequence<I...>) {
    std::size_t constructed = 0;
    try {
      ((::new (static_cast<void*>(return_address<I>())) return_slot<I>{},
        ++constructed),
       ...);
    } catch (...) {
      destroy_returns_upto(constructed);
      throw;
    }
    returns_ready_ = true;
  }

  template <std::size_t I = 0>
  void destroy_returns_upto(std::size_t count) noexcept {
    if constexpr (I < stage_count) {
      if (I < count) {
        return_cursor<I>{}.destroy(returns_storage_.data());
      }
      destroy_returns_upto<I + 1>(count);
    }
  }

  void destroy_returns() noexcept {
    if (!returns_ready_) return;
    destroy_returns_upto<0>(stage_count);
    returns_ready_ = false;
  }

  template <std::size_t... I>
  void copy_returns(function_series const& other, std::index_sequence<I...>) {
    std::size_t constructed = 0;
    try {
      ((::new (static_cast<void*>(return_address<I>()))
            return_slot<I>(other.template return_at<I>()),
        ++constructed),
       ...);
    } catch (...) {
      destroy_returns_upto(constructed);
      throw;
    }
    returns_ready_ = true;
  }

  template <std::size_t... I>
  void move_returns(function_series& other, std::index_sequence<I...>) {
    std::size_t constructed = 0;
    try {
      ((::new (static_cast<void*>(return_address<I>()))
            return_slot<I>(std::move(other.template return_at<I>())),
        ++constructed),
       ...);
    } catch (...) {
      destroy_returns_upto(constructed);
      throw;
    }
    returns_ready_ = true;
  }

  template <std::size_t I, class... Args>
  void bind_stage(std::size_t index, Args&&... args) {
    if (index == I) {
      if constexpr ((std::tuple_element_t<I, stages_type>::template accepts_all<
                         Args>() &&
                     ...)) {
        std::get<I>(stages_).bind_a_lot(std::forward<Args>(args)...);
      } else {
        throw exception{error::type_mismatch};
      }
      return;
    }
    if constexpr (I + 1 < stage_count) {
      bind_stage<I + 1>(index, std::forward<Args>(args)...);
    } else {
      throw exception{error::type_mismatch};
    }
  }

  template <class Arg>
  void bind_flat_one(std::size_t& position, Arg&& arg) {
    if (position >= total_args) throw exception{error::argument_overflow};
    bind_global<0, 0>(position, std::forward<Arg>(arg));
    ++position;
  }

  // 在“第 Stage 段的第 Local 格”与当前扁平槽位比较；命中就绑定。
  template <std::size_t Stage, std::size_t Local, class Arg>
  void bind_global(std::size_t position, Arg&& arg) {
    if (position == stage_start<Stage>() + Local) {
      if constexpr (std::tuple_element_t<Stage,
                                         stages_type>::template slot_accepts<Local,
                                                                             Arg>()) {
        std::get<Stage>(stages_).template bind_at<Local>(std::forward<Arg>(arg));
      } else {
        throw exception{error::type_mismatch};
      }
      return;
    }
    if constexpr (Local + 1 < stage_arity<Stage>()) {
      bind_global<Stage, Local + 1>(position, std::forward<Arg>(arg));
    } else if constexpr (Stage + 1 < stage_count) {
      bind_global<Stage + 1, 0>(position, std::forward<Arg>(arg));
    } else {
      throw exception{error::index_exceed};
    }
  }

  template <std::size_t I>
  void execute_stage() {
    using return_type =
        typename std::tuple_element_t<I, stages_type>::return_type;
    if constexpr (std::is_void_v<return_type>) {
      std::get<I>(stages_).apply_func();
      return_at<I>() = nullptr;
    } else {
      auto value = std::get<I>(stages_).apply_func();
      using slot = return_slot<I>;
      if constexpr (std::is_assignable_v<slot&, return_type&&>) {
        return_at<I>() = std::move(value);
      } else {
        return_cursor<I>{}.destroy(returns_storage_.data());
        ::new (static_cast<void*>(return_address<I>()))
            slot(std::move(value));
      }
    }
  }

  template <std::size_t I>
  void execute_stage_dispatch(std::size_t index) {
    if (index == I) {
      execute_stage<I>();
      return;
    }
    if constexpr (I + 1 < stage_count) {
      execute_stage_dispatch<I + 1>(index);
    } else {
      throw exception{error::index_exceed};
    }
  }

  template <std::size_t I>
  void continue_stage() {
    if constexpr (I < stage_count) {
      if (series_index_ <= I) {
        execute_stage<I>();
        series_index_ = static_cast<std::uint16_t>(I + 1);
      }
      continue_stage<I + 1>();
    }
  }

  // 上一段返回值 append 到第 I 段当前的绑定游标。
  template <std::size_t I>
  void feed_return_into() {
    if constexpr (I > 0) {
      using previous_return =
          typename std::tuple_element_t<I - 1, stages_type>::return_type;
      if constexpr (!std::is_void_v<previous_return> && stage_arity<I>() > 0) {
        // 可拷贝就按值喂一份（保留本段返回值供后续读取），
        // 只能移动（例如 unique_ptr）就把上一段的返回值移过去。
        if constexpr (std::is_copy_constructible_v<previous_return>) {
          std::get<I>(stages_).bind(return_at<I - 1>());
        } else {
          std::get<I>(stages_).bind(std::move(return_at<I - 1>()));
        }
      }
    }
  }

  // 执行第 I 段（先吃上一段返回值），并把 series 游标推进到 I+1。
  template <std::size_t I>
  void advance_stage() {
    static_assert(I < stage_count, "function_series::advance_stage: out of range");
    feed_return_into<I>();
    execute_stage<I>();
    series_index_ = static_cast<std::uint16_t>(I + 1);
  }

  template <std::size_t I>
  void bind_continue_stage() {
    if constexpr (I < stage_count) {
      if (series_index_ <= I) advance_stage<I>();
      bind_continue_stage<I + 1>();
    }
  }

  template <std::size_t I>
  void bind_last_dispatch() {
    if constexpr (I + 1 < stage_count) {
      if (series_index_ == I + 1) {
        feed_return_into<I + 1>();
        return;
      }
      bind_last_dispatch<I + 1>();
    }
  }

  stages_type stages_;
  alignas(returns_layout::alignment)
      std::array<std::byte, returns_layout::bytes> returns_storage_{};
  bool returns_ready_{false};
  std::uint16_t series_index_{0};
  std::uint16_t argc_index_{0};
};

// ============================================================================
// 工厂 / 自由函数
// ============================================================================

// link_f(f0, f1, ...)：每段用 meta_bind::bind 推导签名，然后装进 series。
template <std::size_t Alignment = 0, class... F>
auto link_f(F&&... functions) {
  using series_type =
      function_series<detail::binder_for_t<Alignment, F>...>;
  return series_type{meta_bind::bind<Alignment>(std::forward<F>(functions))...};
}

// link_efb(b0, b1, ...)：直接链接现成的 binder。
template <class... Binders>
auto link_efb(Binders&&... binders) {
  return function_series<std::decay_t<Binders>...>{
      std::forward<Binders>(binders)...};
}

// series_bind(series, args...)：按段顺序把扁平参数灌进各段参数栈。
template <class Series, class... Args>
std::size_t series_bind(Series& series, Args&&... args) {
  return series.bind_flat(std::forward<Args>(args)...);
}

}  // namespace meta_bind

