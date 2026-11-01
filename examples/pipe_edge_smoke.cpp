#include "meta_ios.hpp"
#include "type_safe_aligned_iterator.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <utility>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;
using meta_objects::meta_produce_t;

namespace pipe_edge_tests {

// ============================================================================
// 1) 空源 + wait_for_end：不能冲刷未初始化的 receiver
// ============================================================================
using empty_wait = meta_pipe<meta_istream_list<>>
                       ::all_to<meta_aligned_iterator<>,
                                con<wait_for_end_advance_con<>>>
                       ::run<meta_ostream<>>;

static_assert(exp_utilities::exp_size<typename empty_wait::type::to_t> == 0);

constexpr bool empty_wait_no_callback() {
  unsigned calls = 0;
  empty_wait::for_each([&](auto) { ++calls; });
  return calls == 0;
}
static_assert(empty_wait_no_callback());

using initialized_empty_wait = meta_pipe<meta_istream_list<>>
    ::all_to<meta_ostream<>, con<wait_for_end_advance_con<>>>
    ::run<meta_ostream<>>;
static_assert(std::is_same_v<typename initialized_empty_wait::type::to_t,
                             exp_list<exp_list<>>>);

constexpr bool initialized_empty_wait_once() {
  unsigned calls = 0;
  initialized_empty_wait::for_each([&](auto) { ++calls; });
  return calls == 1;
}
static_assert(initialized_empty_wait_once());

struct scalar_sum_fn {
  template <class Current, class Input>
  using apply = std::integral_constant<int, Current::value + Input::value>;
};
using scalar_sum = meta_objects::meta_object<
  std::integral_constant<int, 0>, scalar_sum_fn>;
using scalar_pipe = meta_pipe<meta_istream_list<
    std::integral_constant<int, 2>, std::integral_constant<int, 3>>>
    ::all_to<scalar_sum, con<wait_for_end_advance_con<>>>
    ::run<meta_iterator>;
static_assert(scalar_pipe::type::to_t::value == 5);

// ============================================================================
// 2) 初始 main 关闭 G_CONTINUE：一轮都不跑
// ============================================================================
using main_off = meta_states_object<condition_seed & ~G_CONTINUE, pipe_main_fn>;

using off_pipe = meta_pipe<meta_istream_list<int, double>>
                     ::all_to<meta_aligned_iterator<>>
                     ::run<meta_ostream<>, con<node_con<>>, main_con<main_off>>;

static_assert(exp_utilities::exp_size<typename off_pipe::type::to_t> == 0);
static_assert(off_pipe::type::states::template get<u16, position_shift> == 0);
static_assert(off_pipe::type{}.index() == 0);

constexpr bool off_pipe_no_callback() {
  unsigned calls = 0;
  off_pipe::for_each([&](auto) { ++calls; });
  return calls == 0;
}
static_assert(off_pipe_no_callback());

struct unused_policy;
using off_with_unused_policy = meta_pipe<meta_istream_list<int>>
    ::run<meta_ostream<>, con<node_con<>>, main_con<main_off>, unused_policy>;
static_assert(off_with_unused_policy::type{}.index() == 0);
static_assert(std::is_void_v<decltype(off_with_unused_policy::for_each_forward(
    [](auto, auto) { return 1; }, 42))>);

struct count_received {
  template <class Flags, class Object>
  using apply = typename Flags::template inc<u16, position_shift>;
};
struct observe_whole_mso {
  template <class Flags, class Rec>
  struct apply {
    static_assert(meta_objects::meta_states_object_t<Rec>);
    static_assert(std::is_same_v<typename Rec::function, count_received>);
    using type = typename Flags::template opDeact<
        (Flags::template get<u16, position_shift> >= 2) ?
            u128{G_CONTINUE} : u128{0}>;
    static constexpr auto value = type::value;
  };
};
using mso_output = meta_pipe<index_sequence_istream<3>>::run<
    meta_states_object<0, count_received>,
    con<node_con<meta_ios_details::unchanged, true>>,
    main_con<meta_states_object<condition_seed, observe_whole_mso>>>;
static_assert(mso_output::type::to::flags::template get<u16, position_shift> == 2);

// ============================================================================
// 3) for_each_forward：已执行回调的返回值不能被丢掉
// ============================================================================
template <bool IsPostGen, class Flags, class Object>
struct filter_step {
  using type = Flags;
};
template <class Flags, class Object>
struct filter_step<true, Flags, Object> {
  using type = std::conditional_t<
      std::is_same_v<int, meta_produce_t<Object>>,
      typename Flags::template opSet<G_RECEIVE |
                                     meta_stream_details::pipe_emit_bit>,
      typename Flags::template opDeact<G_RECEIVE |
                                       meta_stream_details::pipe_emit_bit>>;
};

struct filter_int {
  template <class Window, class Flags, class Object>
  using apply = typename filter_step<Window::value == post_gen_exec, Flags,
                                     Object>::type;
};

using filtered3 = meta_pipe<meta_istream_list<int, double, int>>
                     ::all_to<meta_iterator, con<node_con<filter_int, true>>>
                     ::run<meta_ostream<>>;

// 第一个元素就被过滤：下游这一段不能因为“上游还没有产物”而提前死亡，
// 只能空转，等到后面 int 出现时再接收。
using filtered_lead = meta_pipe<meta_istream_list<double, int, int>>
                          ::all_to<meta_iterator,
                                   con<node_con<filter_int, true>>>
                          ::run<meta_ostream<>>;
static_assert(std::is_same_v<typename filtered_lead::type::to_t,
                             exp_list<int, int>>);

// 第 0 轮命中（用第 1 个参数），第 1 轮被过滤（消耗第 2 个参数），
// 第 2 轮本该命中但没有参数了 —— 整体应返回第 0 轮回调的值。
constexpr int forward_result() {
  return filtered3::for_each_forward(
      [](auto, auto&&) -> int { return 1; }, 10, 20);
}
static_assert(forward_result() == 1);

// ============================================================================
// 4) 自定义 main：低 64 位的用户状态不能被存活位图覆盖
// ============================================================================
struct main_mark_16 {
  template <class Flags, class Rec>
  struct apply {
    using marked_flags = typename Flags::template opSet<(u64{1} << 16)>;
    static constexpr auto value = marked_flags::value;
  };
};

using marked = meta_pipe<meta_istream_list<int, double>>
                   ::all_to<meta_aligned_iterator<>>
                   ::run<meta_ostream<>, con<node_con<>>,
                          main_con<meta_states_object<condition_seed,
                                                       main_mark_16>>>;

static_assert(marked::type::states::template test<(u64{1} << 16)>);

// ============================================================================
// 5) 无限源 + main 主动停止
// ============================================================================
struct stop_after_three {
  template <class Flags, class Rec>
  struct apply {
    static constexpr u16 pos = Flags::template get<u16, position_shift>;
    static constexpr auto value = [] {
      if constexpr (pos >= 3) {
        return Flags::template opDeact<G_CONTINUE>::value;
      } else {
        return Flags::value;
      }
    }();
  };
};

using infinite_stop =
    meta_pipe<meta_index_istream<0>>
        ::all_to<meta_ostream<>>
        ::run<meta_ostream<>, con<node_con<>>,
               main_con<meta_states_object<condition_seed,
                                            stop_after_three>>>;

static_assert(exp_utilities::exp_size<typename infinite_stop::type::to_t> == 3);

// ============================================================================
// 6) 尾部节点死亡：pipe 继续 drain，死亡节点不再被调用
// ============================================================================
struct tail_kill {
  template <class Window, class Flags, class Object>
  using apply = std::conditional_t<
      Window::value == pre_gen_exec && user_fn::index_of<Flags>() == 1,
      typename Flags::template opDeact<G_CONTINUE>, Flags>;
};

using tail_dead =
    meta_pipe<meta_istream_list<int, double, char, int>>
        ::all_to<meta_aligned_iterator<>>
        ::run<meta_iterator, con<node_con<tail_kill, false>>>;

// 尾节点在第 1 轮后死亡，最终 payload 冻结在第一个 seek_to。
static_assert(std::is_same_v<
              typename tail_dead::type::to_t,
              typename meta_invoke<meta_aligned_iterator<>, int>::type>);

// 4 个元素 + 初始化轮 = 5 轮；尾节点死亡后 pipe 仍要把源 drain 完。
static_assert(tail_dead::type::states::template get<u16, position_shift> == 5);
static_assert(tail_dead::type::states::template get<u64, 64> == 0);

constexpr bool tail_dead_callback_once() {
  unsigned calls = 0;
  tail_dead::for_each([&](auto) { ++calls; });
  return calls == 1;
}
static_assert(tail_dead_callback_once());

// ============================================================================
// 7) type_mask 的 128 类型边界（129 由 storage 的 static_assert 拒绝）
// ============================================================================
template <class Seq, std::size_t Target>
struct mask_of;
template <std::size_t... I, std::size_t Target>
struct mask_of<std::index_sequence<I...>, Target> {
  using target = std::integral_constant<std::size_t, Target>;
  static constexpr auto value = type_safe_aligned::type_mask_series<
      target, std::integral_constant<std::size_t, I>...>;
};
// 64 类型：第 63 个命中 -> low 的最高位。
static_assert(mask_of<std::make_index_sequence<64>, 63>::value.low ==
              (u64{1} << 63));
static_assert(mask_of<std::make_index_sequence<64>, 63>::value.high == 0);

// 128 类型：low/high 交界处（第 63、64 个）与高位（第 100、127 个）。
static_assert(mask_of<std::make_index_sequence<128>, 63>::value.low ==
              (u64{1} << 63));
static_assert(mask_of<std::make_index_sequence<128>, 63>::value.high == 0);
static_assert(mask_of<std::make_index_sequence<128>, 64>::value.low == 0);
static_assert(mask_of<std::make_index_sequence<128>, 64>::value.high == 1);
static_assert(mask_of<std::make_index_sequence<128>, 100>::value.low == 0);
static_assert(mask_of<std::make_index_sequence<128>, 100>::value.high ==
              (u64{1} << 36));
static_assert(mask_of<std::make_index_sequence<128>, 127>::value.high ==
              (u64{1} << 63));

static_assert(!std::is_copy_assignable_v<
        type_safe_aligned::aligned_iterator_storage<int>>);

#ifdef META_PIPE_TEST_TOO_MANY_TYPES
template <std::size_t... Index>
auto oversized_storage(std::index_sequence<Index...>)
  -> type_safe_aligned::aligned_iterator_storage<
    std::integral_constant<std::size_t, Index>...>;
static_assert(sizeof(decltype(oversized_storage(
          std::make_index_sequence<129>{}))) > 0);
#endif

}  // namespace pipe_edge_tests

int main() {
  std::cout << "edge cases ok\n";
  return 0;
}
