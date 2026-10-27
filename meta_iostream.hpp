#pragma once

// ============================================================================
// meta_iostream —— 与 meta_stream 无关的 i/ostream 集合
// （从 meta_ios 里提取出来的那一部分：都是"普通元对象"，不依赖 meta_stream）
//
// 每个流都只满足元对象协议：
//   istream : ::type（剩余状态）/ ::ret（当前要吐出的东西）/ apply（推进一步）
//   ostream : ::type（累积结果）/ apply(this_state, from_ins)（吸一个进来）
// 所以它们能和 meta_states::run / meta_states::looper 直接配套，也能和
// 自己写的流互换。
//
// ---------------------------------------------------------------------------
// meta_istream 的语义（带初始化器版）：
//   构造时**不**碰用户的类型列表；第一次被调用时才由 initialize 现场把
//   用户的类型列表**拷贝**成 exp_list 作为 payload，之后 apply 只在 exp_list
//   上推进（pop_front / front）。所以：
//     - 用户的类型列表原样不动；
//     - 可以喂 exp_list，也可以喂 std::tuple / std::variant 这类"类型列表形状"；
//     - 流的初始状态是"未初始化"，由 looper 的无参初始化协议接管。
// ---------------------------------------------------------------------------
//
// meta_ios.hpp 聚合最终 API；本组件层不依赖管道或 API 门面。
// ============================================================================

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <concepts>

#include "meta_objects.hpp"

namespace meta_iostream {

// ===== 常用别名（这一层只吃元对象协议 + exp_utilities 的基础件）=====
using exp_utilities::exp_list;
using exp_utilities::exp_size;
using exp_utilities::get_type;
using exp_utilities::to_exp_list_t;
using exp_utilities::literal_types::end_of_list;
using exp_utilities::literal_types::no_exist_type;
using meta_objects::meta_object;
using meta_objects::meta_ret_object;
using meta_objects::no_arguments;
using meta_objects::this_ref;
using meta_quote::bind_binary;
using meta_quote::binary;
using meta_quote::unary;

// payload 自己声明"我结束了"：looper / 条件就用它判断流到头了，
// 不必去猜 payload 的形状（列表长度、计数器……）。
template <bool End, class EndType = end_of_list>
struct end_of_stream {
  static constexpr bool end = End;
  using end_type = EndType;
};
template<class T>
concept end_of_stream_t = requires{
  typename std::bool_constant<static_cast<bool>(T::end)>;
  typename T::end_type;
};
template<class ret_func>
struct end_of_ret{
  template<class this_obj, class... Args>
  struct apply_impl{
    using type = meta_invoke<ret_func, this_obj, Args...>;
  };
  template<end_of_stream_t this_obj, class... Args>
    requires (static_cast<bool>(this_obj::end))
  struct apply_impl<this_obj, Args...>{
    using type = typename this_obj::end_type;
  };
  template<class this_obj, class... Args>
  using apply = typename apply_impl<this_obj, Args...>::type;
};

template<class TL1, class TL2>
struct reload_to_impl;

template<
  class ...Tys1,
  class ...Tys2,
  template<class ...> class TL1,
  template<class ...> class TL2
>
struct reload_to_impl<TL1<Tys1...>, TL2<Tys2...>>
{
  using type = TL2<Tys1...>;
};

template<class From, class To>
using reload_to = typename reload_to_impl<From, To>::type;

template<class typelist>
struct template_forward{
  template<class...>
  using apply = exp_utilities::literal_types::no_exist_type;
};

template<template<typename ...> class container_, class ...types>
struct template_forward<container_<types...>>{
    template<class ...other_types>
    using container = container_<other_types...>;
    template<class typelist>
    struct container_impl{
        using type = exp_utilities::literal_types::no_exist_type;
    };
    template<template<typename ...> class TL, class ...other_types>
    struct container_impl<TL<other_types...>>{
        using type = container_<other_types...>;
    };
    template<class typelist>
    using apply = typename container_impl<typelist>::type;
};


// ============================================================================
// 1) 基础 istream / ostream
// ============================================================================

namespace basic_istream_detail {
// 带初始化器的推进函数：payload 由 initialize 现场造出来（= 用户列表的 exp_list 拷贝），
// 之后每一步只在 payload 上 pop_front —— 用户自己的类型列表从头到尾没被碰过。
template <class TL>
struct list_dec {
  template <this_ref list, class...>
  using apply = list::pop_front;
  template <no_arguments>
  using initialize = to_exp_list_t<TL>;
};

template <this_ref list>
using front_of = typename list::front;
}  // namespace basic_istream_detail

template <class TL>
using meta_istream =
    meta_ret_object<basic_istream_detail::list_dec<TL>,
                    unary<basic_istream_detail::front_of>>;

template <class... Tys>
using meta_istream_list = meta_istream<exp_list<Tys...>>;

namespace bid_direction_istrema_detail{
  // 迭代 payload：把用户列表的元素原样重绑到 is_list 上，只借它的长度（exp_size）
  template <class ...Tys>
  struct is_list{};

  // 显式终态：原实现让反向的 `next` 从 0 减到 size_t(-1)，再用
  // `I == static_cast<std::size_t>(-1)` 判结束——下标是"回绕出来的"，
  // 而且 is_list_iter<-1> 里还留着 `type = exp_select<-1, TL>` 这样的非法下标。
  // 现在"结束"是一个自己的状态：没有下标、没有越界选择、多推一步还是自己。
  template<class TL>
  struct is_list_end: end_of_stream<true>{
    static constexpr std::size_t length = 0;  // 终态没有剩余元素
    using type = no_exist_type;
    using next = is_list_end;     // 吸收态
    using reverse = is_list_end;  // 终态没有"另一方向的位置"
  };

  template<std::size_t I, bool direction, class TL>
  struct is_list_iter;

  // 正向/反向只在"边界那一格"不同：正向走完 size-1、反向走完 0 之后进终态。
  // 这里用显式分流代替 conditional_t：Ended == true 时走特化，另一个分支的
  // 模板体根本不会实例化，`I - 1` / `I + 1` 连"拼出来"这一步都不会发生，
  // 所以 size_t(-1) 在类型参数里彻底不出现。
  template<bool Ended, std::size_t I, bool direction, class TL>
  struct next_iter{
    using type = is_list_iter<(direction ? I + 1 : I - 1), direction, TL>;
  };
  template<std::size_t I, bool direction, class TL>
  struct next_iter<true, I, direction, TL>{
    using type = is_list_end<TL>;
  };

  // I 是绝对下标：正向 0 -> total-1，反向 total-1 -> 0，
  // 两个方向共用同一套下标语义，只有 next 的结束条件分方向。
  //
  // length 走 exp_size 的长度协议：exp_size 先看 payload 有没有 ::length
  // （meta_utilities 里的 has_length 分支），有就直接用。这里报的是
  // "含当前元素在内还剩几个"，和 meta_istream（pop_front，payload 长度就是
  // 剩余个数）对齐，所以 states.left() 在正反两个方向都随推进递减。
  template<std::size_t I, bool direction, class TL>
  struct is_list_iter: end_of_stream<false>{
    static constexpr std::size_t total = exp_size<TL>;
    static constexpr bool next_ends = direction ? (I + 1 == total) : (I == 0);
    static constexpr std::size_t length = direction ? (total - I) : (I + 1);
    using type = exp_utilities::exp_select_from<0, I, TL>;
    using reverse = is_list_iter<I, !direction, TL>;  // 位置不变，只翻方向
    using next = typename next_iter<next_ends, I, direction, TL>::type;
  };

  template<class TL, bool direction = true>
  struct list_iter_advance_f{
    using payload_t = reload_to<TL, is_list<>>;

    // 空表开局就是终态；非空时正向从 0、反向从 size-1 开始
    template<bool Empty, class P, bool dir>
    struct first_iter{
      using type = is_list_iter<(dir ? 0 : exp_size<P> - 1), dir, P>;
    };
    template<class P, bool dir>
    struct first_iter<true, P, dir>{
      using type = is_list_end<P>;
    };

    template<class this_typelist, class ...Tys>
    using apply = typename this_typelist :: next;
    template<no_arguments>
    using initialize = typename first_iter<(exp_size<payload_t> == 0),
                                           payload_t, direction>::type;
  };

  struct list_ret_f{
    template<class this_typelist>
    using apply = get_type<this_typelist>;
  };
  
}
template<class TL, bool direction = true>
using meta_bistream = meta_objects::meta_ret_object<
  bid_direction_istrema_detail::list_iter_advance_f<TL, direction>,
  end_of_ret<bid_direction_istrema_detail::list_ret_f>>;

namespace basic_ostream_detail {
template <class L, class T>
struct add_impl {
  using type = no_exist_type;
};

template <template <class...> class L, class... Ts, class T>
struct add_impl<L<Ts...>, T> {
  using type = L<Ts..., T>;
};

template <class this_list, class T>
using add_f = typename add_impl<this_list, T>::type;
}  // namespace basic_ostream_detail

template <class TL = exp_list<>>
using meta_ostream = meta_object<TL, binary<basic_ostream_detail::add_f>>;

// ============================================================================
// 2) 变换类
// ============================================================================

namespace transform_istream_detail {
using basic_istream_detail::list_dec;
using basic_istream_detail::front_of;

// 先把 current 的 front 取出，再过一遍用户的 F
template <class F, class this_list>
using pop_transform_f = meta_invoke<F, front_of<this_list>>;
}  // namespace transform_istream_detail

template <class TL, class F>
using meta_transform_istream =
    meta_ret_object<transform_istream_detail::list_dec<TL>,
                    bind_binary<transform_istream_detail::pop_transform_f, F>>;

namespace transform_ostream_detail {
using basic_ostream_detail::add_f;

// 吸收时先 F(this_list, from_ins) 变换，再放进列表
template <class F>
struct add_transform_f {
  template <class this_list, class from_ins>
  using apply =
      add_f<this_list, meta_invoke<F, this_list, from_ins>>;
};
}  // namespace transform_ostream_detail

template <class TL, class F>
using meta_transform_ostream =
    meta_object<TL, transform_ostream_detail::add_transform_f<F>>;

// ============================================================================
// 3) 收集方式类 ostream
// ============================================================================

namespace join_ostream_detail {
using basic_ostream_detail::add_f;

template <class T>
struct add_to_this_list {
  template <class this_list>
  using apply = add_f<this_list, T>;
};

template <class this_list, class T>
struct auto_join_f {
  using type = add_f<this_list, T>;
};

// 读进来本身是个类型列表 → 把它摊开逐个并入（而不是把列表当成一个元素）
template <class this_list, template <class...> class L, class... Tys>
struct auto_join_f<this_list, L<Tys...>> {
  using type = meta_fold<this_list, add_to_this_list<Tys>::template apply...>;
};

struct auto_join {
  template <class this_list, class T>
  using apply = typename auto_join_f<this_list, T>::type;
};
}  // namespace join_ostream_detail

template <class TL>
using meta_jostream = meta_object<TL, join_ostream_detail::auto_join>;

namespace reverse_ostream_detail {
// add_front：把 T 插到任意"类型列表形状"的前面，保留它原来的模板
//（不会强行变成 exp_list）。它是 basic_ostream 的 add 的"从前面插"版本。
template <class TL, class T>
struct add_front_impl {
  using type = no_exist_type;
};

template <template <class...> class L, class... Tys, class T>
struct add_front_impl<L<Tys...>, T> {
  using type = L<T, Tys...>;
};

template <class TL, class T>
using add_front_f = typename add_front_impl<TL, T>::type;

// 每步都往前面插，所以最终列表是"到达顺序的逆序"（适合逆序析构）
struct reverse_collect_f {
  template <class this_list, class from_ins>
  using apply = add_front_f<this_list, from_ins>;
};
}  // namespace reverse_ostream_detail

template <class TL = exp_list<>>
using meta_rostream = meta_object<TL, reverse_ostream_detail::reverse_collect_f>;

namespace filter_ostream_detail {
using basic_ostream_detail::add_f;

// 谓词为真才收下，否则这一步列表不动
template <class pred>
struct add_filter_f {
  template <class this_list, class from_ins>
  using apply = std::conditional_t<
      meta_invoke<pred, this_list, from_ins>::value,
      add_f<this_list, from_ins>, this_list>;
};
}  // namespace filter_ostream_detail

template <class TL, class pred>
using meta_filter_ostream =
    meta_object<TL, filter_ostream_detail::add_filter_f<pred>>;

namespace replace_able_ostream_detail {
template <class this_obj, class T>
using replace_this = T;

// 每次直接把状态换成读进来的东西（"可替换状态"的 ostream）
using replace_able_ostream = meta_object<no_exist_type, binary<replace_this>>;
}  // namespace replace_able_ostream_detail

using meta_iterator = replace_able_ostream_detail::replace_able_ostream;

namespace transform_iterator_detail {
template <class F>
struct transform_iterator_f {
  template <class this_obj, class from_ins>
  using apply = meta_invoke<F, this_obj, from_ins>;

  template <class from_ins> requires(!no_arguments<from_ins>)
  using initialize = meta_objects::initialize<F, from_ins>;
};

template <bool con, meta_objects::meta_object_t mo, class T>
using set_if =
    std::conditional_t<con, typename mo::template meta_set<T>, mo>;
}  // namespace transform_iterator_detail

template <class F, class init = no_exist_type>
using meta_transform_iterator =
    transform_iterator_detail::set_if<
        !std::is_same_v<no_exist_type, init>,
        meta_object<transform_iterator_detail::transform_iterator_f<F>>, init>;

namespace forward_ostream_detail {
using exp_utilities::exp_select;

template <std::size_t L, class TL, class F>
struct forward_ostream_f_impl {
  // 按当前列表长度取索引，把 F 作用在"对应位置的类型"和读进来的参数上
  // no_exist if TL is not list;
  using reserve_typelist = template_forward<TL>;
  template <class this_list, class Arg>
  struct apply {
    using op_list = to_exp_list_t<this_list>;
    static constexpr std::size_t current_index =
        to_exp_list_t<this_list>::length;
    using result = meta_invoke<F, exp_select<current_index, TL>, Arg>;
    using type =
        meta_invoke<reserve_typelist, typename op_list::template push_back<result>>;
  };
  // 有些流不会把自己读空，长度到 L 之后就不再应用 F（避免越界）
  template <class this_list, class Arg>
    requires(to_exp_list_t<this_list>::length >= L)
  struct apply<this_list, Arg> {
    using type = meta_invoke<reserve_typelist, typename to_exp_list_t<this_list>::template push_back<Arg>>;
  };
};

template <class TL, class F>
struct forward_ostream_f {
  template <class this_list, class Arg>
  using apply = typename forward_ostream_f_impl<exp_size<TL>, TL, F>::
      template apply<this_list, Arg>::type;
};
}  // namespace forward_ostream_detail

template <class T1, class T2>
using default_combine = exp_list<T1, T2>;

template <class TL, class F = binary<default_combine>>
using meta_forward_ostream = meta_object<
    exp_list<>, forward_ostream_detail::forward_ostream_f<TL, F>>;

// ============================================================================
// 4) 生成的 istream（索引 / 重复）
// ============================================================================

namespace index_istream_detail {
using exp_utilities::Idx;
using exp_utilities::inc_idx_t;

// payload 是"一个元素的 exp_list"：exp_size == 1 所以流永远不会因为空而结束
template <std::size_t start>
using my_counter = exp_list<Idx<start>>;

template <class this_list>
using make_inc_t = exp_list<inc_idx_t<typename this_list::template at<0>>>;

template <class this_list>
using ret_index = typename this_list::template at<0>;
}  // namespace index_istream_detail

// 永不结束：每次吐出递增的下标
template <std::size_t start>
using meta_index_istream =
    meta_ret_object<index_istream_detail::my_counter<start>,
                    unary<index_istream_detail::make_inc_t>,
                    end_of_ret<unary<index_istream_detail::ret_index>>>;

namespace index_sequence_istream_detail {
template <std::size_t dec_index>
struct index_counter : std::integral_constant<std::size_t, dec_index>,
                       end_of_stream<(dec_index == 0)> {
  static constexpr std::size_t length = dec_index;
  using dec_t = index_counter<dec_index - 1>;
};

template <std::size_t Len>
struct dec_index_f {
  template <class this_index, class...>
  using apply = typename this_index::dec_t;
};

template <std::size_t Len>
struct ret_index {
  // 吐出的位置归一化成 std::integral_constant（而不是状态类型本身），
  // 这样 is_same<cache, integral_constant<size_t, K>> 这类精确判定能命中。
  // 循环状态仍然是 index_counter。
  template <class this_index, class...>
  using apply = std::integral_constant<std::size_t, Len - this_index::value>;
};
}  // namespace index_sequence_istream_detail

// 有界：依次吐 0, 1, ..., Len-1，然后自然结束（靠 end_of_stream）
template <std::size_t Len>
using index_sequence_istream =
    meta_ret_object<index_sequence_istream_detail::index_counter<Len>,
                    index_sequence_istream_detail::dec_index_f<Len>,
                    end_of_ret<index_sequence_istream_detail::ret_index<Len>>>;

namespace repeat_istream_detail {
template <class this_obj>
using ret_self = typename this_obj::template at<0>;

template <class this_obj>
using do_nothing = this_obj;
}  // namespace repeat_istream_detail

// 永不结束：每次都吐同一个 T
template <class T>
using meta_repeat_istream =
    meta_ret_object<exp_list<T>, unary<repeat_istream_detail::do_nothing>,
                    unary<repeat_istream_detail::ret_self>>;

// ============================================================================
// 5) 运行期字节布局：meta_aligned_iterator
// ============================================================================

namespace aligned_iterator_detail {
template <std::size_t start, class from_ins>
struct seek_to {
  static constexpr std::size_t align = alignof(from_ins);
  // O(1)：把 start 向上取整到 align 的倍数（alignof 一定是 2 的幂）
  static constexpr std::size_t value = (start + align - 1) & ~(align - 1);
  using advance_t = std::integral_constant<std::size_t, value + sizeof(from_ins)>;
  using type = from_ins;

  void emplace(std::byte* ptr, type const& val) { new (ptr + value) type{val}; }
  void emplace(std::byte* ptr, type&& val) {
    new (ptr + value) type{std::move(val)};
  }
  void destroy(std::byte* ptr) {
    std::destroy_at(reinterpret_cast<type*>(ptr + value));
  }
  std::byte* address_of(std::byte* base_ptr) const { return base_ptr + value; }
  type& get(std::byte* ptr) { return *reinterpret_cast<type*>(ptr + value); }
  type const& c_get(const std::byte* ptr) const {
    return *reinterpret_cast<const type*>(ptr + value);
  }
};

struct advance_f {
  template <class this_seek, class from_is>
  using apply = seek_to<this_seek::advance_t::value, from_is>;
  // 初始化：只吃真正的类型。必须用 !no_arguments 挡掉"无参哨兵" ——
  // 否则 initialize 会被判定成"可无参初始化"，把哨兵当成第一个元素类型。
  template <class from_is>
    requires(!no_arguments<from_is>)
  using initialize = seek_to<0, from_is>;
};
}  // namespace aligned_iterator_detail

// 把类型流映射成"对齐后的字节偏移"：状态就是个 seek_to，
// 运行期可以用 emplace / get / destroy / address_of 在那个偏移上干活。
// 用法只有一种：调用元对象本身。想要"某一格上的迭代器"就 meta_set：
//   meta_invoke<meta_aligned_iterator, T>::type          → 第一格（seek_to<0, T>）
//   meta_invoke<meta_aligned_iterator::meta_set<back>, T> → 接着 back 往下推进
using meta_aligned_iterator =
    meta_object<aligned_iterator_detail::advance_f>;

}  // namespace meta_iostream
