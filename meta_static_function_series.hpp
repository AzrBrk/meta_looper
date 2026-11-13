#pragma once

#include "meta_function_binder.hpp"
#include "meta_ios.hpp"

#include <memory>
#include <stdexcept>

namespace meta_bind {
namespace static_series_detail {

struct import_binders_t {};

template <class F, class R, class Parameters>
struct imported_callable;

template <class F, class R, class... P>
struct imported_callable<F, R, exp_list<P...>> {
  using imported_signature = detail::function_pointer_traits<R (*)(P...)>;
  F callable;
  template <class... Args>
  decltype(auto) operator()(Args&&... values) {
    return std::invoke(callable, std::forward<Args>(values)...);
  }
};

template <class Binder>
using binder_callable = imported_callable<
    typename std::remove_cvref_t<Binder>::callable_type,
    typename std::remove_cvref_t<Binder>::return_type,
    typename std::remove_cvref_t<Binder>::argument_types>;

template <class Binder>
decltype(auto) import_callable(Binder&& binder) {
  if constexpr (std::is_lvalue_reference_v<Binder&&>)
    return (binder.callable());
  else
    return std::move(binder.callable());
}

template <std::size_t Index>
struct argument_condition {
  template <class Window, class Flags, class Object>
  struct apply {
    static constexpr auto value = [] {
      constexpr auto position = meta_states::user_fn::index_of<Flags>();
      auto flags = Flags::value;
      if constexpr (position == Index)
        flags = flags | meta_objects::u128{meta_states::G_NOT_SKIP};
      else
        flags = flags & ~meta_objects::u128{meta_states::G_NOT_SKIP};
      if constexpr (position == Index + 1)
        flags = flags & ~meta_objects::u128{meta_states::G_CONTINUE};
      return flags;
    }();
  };
};

template <std::size_t Index, class... Args>
decltype(auto) select_argument(Args&&... args) {
  return meta_states::run<
      meta_ios::stream_states_co<argument_condition<Index>>,
      meta_iostream::meta_iterator,
      meta_iostream::index_sequence_istream<sizeof...(Args)>>::for_each_forward(
          [](auto, auto&& value) -> decltype(auto) {
            return std::forward<decltype(value)>(value);
          }, std::forward<Args>(args)...);
}

template <class F>
using callable_info = typename decltype([] {
  if constexpr (requires { typename F::imported_signature; })
    return std::type_identity<typename F::imported_signature>{};
  else if constexpr (std::is_pointer_v<F>)
    return std::type_identity<detail::function_pointer_traits<F>>{};
  else
    return std::type_identity<exp_function_info<F>>{};
}())::type;

template <class... F>
using parameters = typename meta_states::run<
    meta_ios::stream_states_co<>, meta_iostream::meta_jostream<exp_list<>>,
    meta_iostream::meta_istream_list<typename callable_info<F>::argument_types...>>
    ::type::to_t;

template <class List>
struct storage_layout;

template <class... T>
struct storage_layout<exp_list<T...>> : detail::aligned_slot_layout<0,
    select_if_t<sizeof...(T) == 0, always_fn<exp_list<detail::no_argument_slot>>,
                always_fn<exp_list<T...>>>> {};

template <class R>
using result_slot = typename decltype([] {
  if constexpr (std::is_void_v<R>)
    return std::type_identity<detail::no_argument_slot>{};
  else
    return std::type_identity<std::remove_cvref_t<R>>{};
}())::type;

template <class Layout, std::size_t Index>
struct slot_position : Layout::template cursor<Index> {
  static constexpr std::size_t index = Index;
};

template <class Layout, std::size_t Begin, class Index>
struct pair_frame {
  using main = slot_position<Layout, Begin + 2 * Index::value>;
  using branch = slot_position<Layout, Begin + 2 * Index::value + 1>;
};

template <class Layout, std::size_t Begin, bool Branch>
struct pair_receiver {
 private:
  template <class Index>
  using project = typename decltype([] {
    using frame = pair_frame<Layout, Begin, Index>;
    if constexpr (Branch) return std::type_identity<typename frame::branch>{};
    else return std::type_identity<typename frame::main>{};
  }())::type;

 public:
  template <class Index>
    requires (!meta_objects::no_arguments<Index>)
  using initialize = project<Index>;

  template <class Previous, class Index>
  using apply = project<Index>;
};

template <class Layout, std::size_t Begin, std::size_t Count, class Op, class... Args>
void dispatch(Op&& operation, Args&&... args) {
  if constexpr (Count >= 2) {
    using pipeline = typename meta_ios::meta_pipe<
        meta_iostream::index_sequence_istream<Count / 2>>
        ::template all_to<meta_objects::meta_object<pair_receiver<Layout, Begin, false>>>
        ::template fork<meta_objects::meta_object<pair_receiver<Layout, Begin, true>>>
        ::template run<>;
    pipeline::for_each([&](auto view, auto&&... values) {
      operation(view.main(), std::forward<decltype(values)>(values)...);
      operation(view.branch(), std::forward<decltype(values)>(values)...);
    }, std::forward<Args>(args)...);
  }
  if constexpr (Count % 2 != 0)
    operation(slot_position<Layout, Begin + Count - 1>{},
          std::forward<Args>(args)...);
}

template <class Layout>
struct alignas(Layout::alignment) slot_buffer {
  std::array<std::byte, Layout::bytes> bytes;
};

template <class Arguments, class Results, class Callables>
struct inline_storage {
  static constexpr bool movable = false;
  slot_buffer<Arguments> arguments_buffer;
  slot_buffer<Results> results_buffer;
  slot_buffer<Callables> callables_buffer;
  std::byte* arguments() noexcept { return arguments_buffer.bytes.data(); }
  std::byte* results() noexcept { return results_buffer.bytes.data(); }
  std::byte* callables() noexcept { return callables_buffer.bytes.data(); }
  bool available() const noexcept { return true; }
};

template <class Arguments, class Results, class Callables>
struct heap_storage {
  static constexpr bool movable = true;
  using layout = storage_layout<exp_list<slot_buffer<Callables>,
      slot_buffer<Arguments>, slot_buffer<Results>>>;
  struct alignas(layout::alignment) arena {
    std::byte bytes[layout::bytes];
  };
  std::unique_ptr<arena> owner{new arena};

  template <std::size_t Index>
  std::byte* region() noexcept {
    if (!owner) return nullptr;
    return owner->bytes + layout::template cursor<Index>::value;
  }
  std::byte* arguments() noexcept { return region<1>(); }
  std::byte* results() noexcept { return region<2>(); }
  std::byte* callables() noexcept { return region<0>(); }
  bool available() const noexcept { return static_cast<bool>(owner); }
};

template <template <class, class, class> class Storage,
          class Parameters, class... F>
class series;

// 类模板名里不再带 std::index_sequence<Stage...>：段号由 looper 的
// index_sequence_istream / for_each(_forward) 在模板内部产生，实例化名字
// 只跟 Storage / 参数列表 / 函数列表有关。
template <template <class, class, class> class Storage,
          class... P, class... F>
class series<Storage, exp_list<P...>, F...> {
  static_assert((!std::is_reference_v<typename callable_info<F>::return_type> && ...),
                "static_function_series: reference returns are not supported");
  using parameter_list = exp_list<P...>;
  using functions = exp_list<F...>;
  using layout = storage_layout<exp_list<detail::slot_type_t<P>...>>;
  using result_layout = storage_layout<exp_list<
      result_slot<typename callable_info<F>::return_type>...>>;
  using callable_layout = storage_layout<functions>;
  using storage_type = Storage<layout, result_layout, callable_layout>;

  static constexpr auto starts = [] {
    std::array<std::size_t, sizeof...(F) + 1> offsets{};
    std::array<std::size_t, sizeof...(F)> counts{
        exp_size<typename callable_info<F>::argument_types>...};
    for (std::size_t index = 0; index < counts.size(); ++index)
      offsets[index + 1] = offsets[index] + counts[index];
    return offsets;
  }();

 public:
  static constexpr std::size_t total_args = sizeof...(P);
  static constexpr std::size_t stage_count = sizeof...(F);
  using range_type = typename layout::range_type;
  using returns_type = typename result_layout::range_type;

  template <class... Callable>
    requires std::is_same_v<exp_list<std::decay_t<Callable>...>, functions>
  explicit series(Callable&&... callable) {
    try {
      if constexpr (stage_count > 0) {
        stage_loop<>::for_each_forward(
            [this](auto stream, auto&& value) {
              constexpr std::size_t index = stream.index();
              construct_callable<index>(std::forward<decltype(value)>(value));
            },
            std::forward<Callable>(callable)...);
      }
    } catch (...) {
      destroy_callables();
      throw;
    }
  }

  template <class... Binder> requires(sizeof...(Binder) == stage_count)
  explicit series(import_binders_t, Binder&&... binder) {
    (static_cast<void>(binder.live_arguments()), ...);
    if constexpr (stage_count > 0) {
      stage_loop<>::for_each_forward(
          [](auto stream, auto&& value) {
            constexpr std::size_t index = stream.index();
            if constexpr (index > 0) {
              using previous = callable_info<exp_select<index - 1, functions>>;
              if constexpr (!std::is_void_v<typename previous::return_type> &&
                            starts[index] != starts[index + 1]) {
                if (!value.front_feed())
                  throw std::invalid_argument(
                      "link_static_binders: consumer must use front_feed");
              }
            }
          },
          std::forward<Binder>(binder)...);
    }
    try {
      if constexpr (stage_count > 0) {
        stage_loop<>::for_each_forward(
            [this](auto stream, auto&& value) {
              constexpr std::size_t index = stream.index();
              construct_callable<index>(exp_select<index, functions>{
                  import_callable(std::forward<decltype(value)>(value))});
            },
            std::forward<Binder>(binder)...);
        stage_loop<>::for_each_forward(
            [this](auto stream, auto&& value) {
              constexpr std::size_t index = stream.index();
              import_arguments<index>(std::forward<decltype(value)>(value));
            },
            std::forward<Binder>(binder)...);
      }
      if constexpr (total_args == 0) {
        using cursor = typename layout::template cursor<0>;
        ::new (static_cast<void*>(cursor{}.address_of(storage_.arguments())))
            typename cursor::type();
        ++argument_count_;
      }
      if constexpr (stage_count > 0) {
        stage_loop<>::for_each([this](auto stream) {
          constexpr std::size_t index = stream.index();
          construct_result<index>();
        });
      }
      initialized_ = true;
    } catch (...) {
      destroy_results();
      destroy_arguments();
      destroy_callables();
      throw;
    }
  }

  series(series const&) = delete;
  series(series&&) requires (!storage_type::movable) = delete;
  series(series&& other) noexcept requires storage_type::movable
      : storage_(std::move(other.storage_)),
        argument_count_(std::exchange(other.argument_count_, 0)),
        result_count_(std::exchange(other.result_count_, 0)),
        callable_count_(std::exchange(other.callable_count_, 0)),
        initialized_(std::exchange(other.initialized_, false)) {}
  series& operator=(series const&) = delete;
  series& operator=(series&&) requires (!storage_type::movable) = delete;
  series& operator=(series&& other) noexcept requires storage_type::movable {
    if (this != std::addressof(other)) {
      destroy_results();
      destroy_arguments();
      destroy_callables();
      storage_ = std::move(other.storage_);
      argument_count_ = std::exchange(other.argument_count_, 0);
      result_count_ = std::exchange(other.result_count_, 0);
      callable_count_ = std::exchange(other.callable_count_, 0);
      initialized_ = std::exchange(other.initialized_, false);
    }
    return *this;
  }

  ~series() {
    destroy_results();
    destroy_arguments();
    destroy_callables();
  }

  template <class... Args>
    requires(sizeof...(Args) <= total_args)
  void initialize(Args&&... args) {
    if (!storage_.available()) throw exception{error::not_fully_bound};
    if (initialized_) throw exception{error::argument_overflow};
    try {
      dispatch<layout, 0, total_args>([this](auto cursor, auto&&... values) {
        constexpr auto index = decltype(cursor)::index;
        using parameter = exp_select<index, parameter_list>;
        if constexpr (index < sizeof...(Args)) {
          auto&& value = select_argument<index>(std::forward<decltype(values)>(values)...);
          if constexpr (detail::is_ref_slot_v<parameter>) {
            static_assert(detail::slot_bindable_v<parameter, decltype(value)>);
            ::new (static_cast<void*>(cursor.address_of(storage_.arguments())))
              typename decltype(cursor)::type(detail::slot_address_from<parameter>(
                std::forward<decltype(value)>(value)));
          } else {
            ::new (static_cast<void*>(cursor.address_of(storage_.arguments())))
              typename decltype(cursor)::type(std::forward<decltype(value)>(value));
          }
        } else {
          static_assert(!detail::is_ref_slot_v<parameter>,
                        "missing reference argument");
          static_assert(std::is_default_constructible_v<typename decltype(cursor)::type>,
                        "missing argument must be default constructible");
            ::new (static_cast<void*>(cursor.address_of(storage_.arguments())))
              typename decltype(cursor)::type();
        }
        ++argument_count_;
      }, std::forward<Args>(args)...);
      if constexpr (total_args == 0) {
        using cursor = typename layout::template cursor<0>;
        ::new (static_cast<void*>(cursor{}.address_of(storage_.arguments())))
            typename cursor::type();
        ++argument_count_;
      }
      if constexpr (stage_count > 0) {
        stage_loop<>::for_each([this](auto stream) {
          constexpr std::size_t index = stream.index();
          construct_result<index>();
        });
      }
      initialized_ = true;
    } catch (...) {
      destroy_results();
      destroy_arguments();
      throw;
    }
  }

  template <std::size_t Begin = 0, class... Args>
    requires(Begin <= total_args && sizeof...(Args) <= total_args - Begin)
  void write(Args&&... args) {
    check_initialized();
    dispatch<layout, Begin, sizeof...(Args)>([this](auto cursor, auto&&... values) {
      constexpr auto index = decltype(cursor)::index;
      using parameter = exp_select<index, parameter_list>;
      auto&& value = select_argument<index - Begin>(std::forward<decltype(values)>(values)...);
      if constexpr (detail::is_ref_slot_v<parameter>) {
        static_assert(detail::slot_bindable_v<parameter, decltype(value)>);
        cursor.get(storage_.arguments()) = detail::slot_address_from<parameter>(
            std::forward<decltype(value)>(value));
      } else {
        cursor.get(storage_.arguments()) = std::forward<decltype(value)>(value);
      }
    }, std::forward<Args>(args)...);
  }

  range_type arguments() & {
    check_initialized();
    return range_type{storage_.arguments()};
  }

  template <std::size_t Index> requires(Index < total_args)
  decltype(auto) argument() & { return arguments().template get<Index>(); }

  returns_type returns() & {
    check_initialized();
    return returns_type{storage_.results()};
  }

  template <std::size_t Index> requires(Index < stage_count)
  decltype(auto) call() {
    check_initialized();
    using info = callable_info<exp_select<Index, functions>>;
    if constexpr (std::is_void_v<typename info::return_type>) {
      invoke<Index>(typename info::argument_types{});
    } else {
      auto& cached = returns().template get<Index>();
      cached = invoke<Index>(typename info::argument_types{});
      return (cached);
    }
  }

  template <std::size_t Begin = 0> requires(Begin <= stage_count)
  void execute() {
    check_initialized();
    if constexpr (Begin < stage_count) {
      stage_loop<Begin>::for_each([this](auto stream) {
        constexpr std::size_t index = Begin + stream.index();
        call<index>();
      });
    }
  }

  template <std::size_t Begin = 0> requires(Begin <= stage_count)
  void execute_feed() {
    check_initialized();
    if constexpr (Begin < stage_count) {
      stage_loop<Begin>::for_each([this](auto stream) {
        constexpr std::size_t index = Begin + stream.index();
        call<index>();
        if constexpr (index + 1 < stage_count) {
          using info = callable_info<exp_select<index, functions>>;
          if constexpr (!std::is_void_v<typename info::return_type> &&
                        starts[index + 1] != starts[index + 2]) {
            auto& cached = returns().template get<index>();
            using parameter = exp_select<starts[index + 1], parameter_list>;
            if constexpr (detail::is_ref_slot_v<parameter> ||
                          std::is_assignable_v<detail::slot_type_t<parameter>&,
                                               decltype(cached)>)
              write<starts[index + 1]>(cached);
            else
              write<starts[index + 1]>(std::move(cached));
          }
        }
      });
    }
  }

 private:
  template <std::size_t Index, class Binder>
  void import_arguments(Binder&& binder) {
    auto source = binder.live_arguments();
    dispatch<layout, starts[Index], starts[Index + 1] - starts[Index]>(
        [&](auto cursor) {
      constexpr auto local = decltype(cursor)::index - starts[Index];
      auto& value = source.template get<local>();
      if constexpr (std::is_lvalue_reference_v<Binder&&>) {
        ::new (static_cast<void*>(cursor.address_of(storage_.arguments())))
            typename decltype(cursor)::type(value);
      } else {
        ::new (static_cast<void*>(cursor.address_of(storage_.arguments())))
            typename decltype(cursor)::type(std::move(value));
      }
      ++argument_count_;
    });
  }

  void check_initialized() const {
    if (!initialized_) throw exception{error::not_fully_bound};
  }

  // 逐段推进的循环入口：只是 run<...> 的类型别名，没有额外的运行期包装。
  //   - 一轮 = 一个段号，回调里 constexpr std::size_t index = stream.index()
  //     就是本轮的段号（Begin + 轮号），所以分支全部是 if constexpr；
  //   - for_each_forward 逐轮把实参往里推，某一轮是否丢掉队首实参由该轮的
  //     G_CONSUME 决定（consume_ = pre_rec_gates & gate_consume）；consume_ 为假
  //     时同一个实参原样传给下一轮。stream_states_co<> 的种子门
  //     （meta_states_looper_default）里 G_CONSUME 恒开，所以第 i 轮拿到的就是
  //     第 i 个实参；被跳过的轮次同样消耗一个实参 —— 这也是 select_argument
  //     能靠位置取参的原因。
  //   - 需要额外的门/分支条件就写在 stream_states_co<Fn> 里（见 examples/print.hpp），
  //     不要在外面再套一层运行期 helper。
  template <std::size_t Begin = 0>
  using stage_loop = meta_states::run<
      meta_ios::stream_states_co<>, meta_iostream::meta_iterator,
      meta_iostream::index_sequence_istream<sizeof...(F) - Begin>>;

  // ---- 一次调用：让 aligned iterator 沿它自己造出来的布局走一遍 -----------
  // 布局里每一格的地址就是 meta_aligned_iterator 的产物（seek_to<offset, slot>），
  // 所以不需要再造 slot<I>：
  //   - 迭代器从"这一段前面那一格"续上，随后它自己推进到段首、逐格往下；
  //   - 接收器（ostream）逐轮把 seek（offset + 槽位类型）收进 exp_list；
  //   - 每轮的 this_list::length 正好是"这一段里的第几个参数"，用它从声明类型
  //     列表里取出 Parameter，把"声明形状 + 布局游标"配成 call_slot；
  //   - 收完的 exp_list<call_slot<...>...> 一次性摊给 std::invoke。
  // 全程没有 starts[Index] + k，也没有 std::index_sequence / index_sequence_istream。
  template <class Parameter, class Seek>
  struct call_slot {
    static decltype(auto) get(std::byte* base) {
      return detail::call_argument<Parameter>(Seek{}.get(base));
    }
  };

  template <class Parameters>
  struct collect_call_slot {
    // this_list::length 是"已收到的格子数"，也就是当前参数在本段里的下标。
    template <class this_list, class Seek>
    using apply = call_slot<exp_select<this_list::length, Parameters>, Seek>;
  };

  // 段首之前的迭代器状态：布局游标（seek_to）本身就是迭代器的产物。
  // starts[Index] 是本段第一个参数在整张布局里的槽位号，所以"前一格"是
  // starts[Index] - 1；starts[Index] == 0（本段就是布局开头）时用未初始化的
  // 迭代器，让首轮 initialize 从偏移 0 起算。
  template <std::size_t Index>
  using stage_iterator = typename decltype([] {
    if constexpr (starts[Index] == 0) {
      return std::type_identity<meta_iostream::meta_aligned_iterator<>>{};
    } else {
      return std::type_identity<
          typename meta_iostream::meta_aligned_iterator<>::template meta_set<
              typename layout::template cursor<starts[Index] - 1>>>{};
    }
  }())::type;

  template <std::size_t Index, class... Parameter>
  using call_slots = typename meta_ios::meta_pipe<
      meta_iostream::meta_istream_list<detail::slot_type_t<Parameter>...>>
      ::template all_to<stage_iterator<Index>>
      ::template run<meta_iostream::meta_transform_ostream<
          exp_list<>, collect_call_slot<exp_list<Parameter...>>>>
      ::type::to_t;

  template <std::size_t Index>
  decltype(auto) callable_at() {
    return typename callable_layout::template cursor<Index>{}.get(
        storage_.callables());
  }

  template <std::size_t Index, class... Slot>
  decltype(auto) invoke_slots(std::byte* base, exp_list<Slot...>) {
    return std::invoke(callable_at<Index>(), Slot::get(base)...);
  }

  template <std::size_t Index, class... Parameter>
  decltype(auto) invoke(exp_list<Parameter...>) {
    if constexpr (sizeof...(Parameter) == 0) {
      return std::invoke(callable_at<Index>());
    } else {
      std::byte* base = storage_.arguments();
      return invoke_slots<Index>(base, call_slots<Index, Parameter...>{});
    }
  }

  template <std::size_t Index, class Value>
  void construct_callable(Value&& callable) {
    typename callable_layout::template cursor<Index>{}.emplace(
        storage_.callables(), std::forward<Value>(callable));
    ++callable_count_;
  }

  template <std::size_t Index>
  void construct_result() {
    using cursor = typename result_layout::template cursor<Index>;
    static_assert(std::is_default_constructible_v<typename cursor::type>);
    ::new (static_cast<void*>(cursor{}.address_of(storage_.results())))
      typename cursor::type();
    ++result_count_;
  }

  template <class Layout, std::size_t Count>
  static void destroy(std::byte* base, std::size_t& live) noexcept {
    if constexpr (Count > 0) {
      meta_states::run<meta_ios::stream_states_co<>, meta_iostream::meta_iterator,
          meta_iostream::index_sequence_istream<Count>>::for_each(
          [base, &live](auto stream) {
        constexpr auto reverse = Count - 1 - stream.index();
        if (reverse < live)
          typename Layout::template cursor<reverse>{}.destroy(base);
          });
    }
    live = 0;
  }

  void destroy_arguments() noexcept {
    destroy<layout, layout::slot_count>(storage_.arguments(), argument_count_);
  }
  void destroy_results() noexcept {
    destroy<result_layout, stage_count>(storage_.results(), result_count_);
  }
  void destroy_callables() noexcept {
    destroy<callable_layout, stage_count>(storage_.callables(), callable_count_);
  }

  storage_type storage_;
  std::size_t argument_count_{};
  std::size_t result_count_{};
  std::size_t callable_count_{};
  bool initialized_{};
};

}

template <class... F>
using static_function_series = static_series_detail::series<
  static_series_detail::inline_storage,
    static_series_detail::parameters<F...>, F...>;

template <class... F>
using heap_static_function_series = static_series_detail::series<
  static_series_detail::heap_storage,
  static_series_detail::parameters<F...>, F...>;

template <class... F> requires(sizeof...(F) > 0)
auto link_static_heap(F&&... callable) {
  return heap_static_function_series<std::decay_t<F>...>(std::forward<F>(callable)...);
}

template <class... F> requires(sizeof...(F) > 0)
auto link_static(F&&... callable) {
  return static_function_series<std::decay_t<F>...>(std::forward<F>(callable)...);
}

template <class... Binder> requires(sizeof...(Binder) > 0)
auto link_static_binders(Binder&&... binder) {
  return static_function_series<static_series_detail::binder_callable<Binder>...>(
      static_series_detail::import_binders_t{}, std::forward<Binder>(binder)...);
}

template <class... Binder> requires(sizeof...(Binder) > 0)
auto link_static_heap_binders(Binder&&... binder) {
  return heap_static_function_series<static_series_detail::binder_callable<Binder>...>(
      static_series_detail::import_binders_t{}, std::forward<Binder>(binder)...);
}

}