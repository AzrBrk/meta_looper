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

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

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

// 两个分支都做成 thunk：谓词为假时 add_f 不会被实例化。
struct add_pair_fn {
  template <class this_list, class from_ins>
  using apply = add_f<this_list, from_ins>;
};
struct keep_list_fn {
  template <class this_list, class from_ins>
  using apply = this_list;
};

// 谓词为真才收下，否则这一步列表不动
template <class pred>
struct add_filter_f {
  template <class this_list, class from_ins>
  using apply = select_if_t<meta_invoke<pred, this_list, from_ins>::value,
                            add_pair_fn, keep_list_fn, this_list, from_ins>;
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

// mo::meta_set<T> 只在 con 为真时才展开
template <class mo>
struct meta_set_fn {
  template <class T>
  using apply = typename mo::template meta_set<T>;
};

template <bool con, meta_objects::meta_object_t mo, class T>
using set_if = select_if_t<con, meta_set_fn<mo>, always_fn<mo>, T>;
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

// 弱对齐 CPU（老 ARM、MIPS、SPARC、默认 -mstrict-align 的 RISC-V…）不能访问未
// 对齐的对象：alignment < alignof(T) 的布局（典型是 packed = 1）在那些机器上
// 是 UB，严重时直接 fault。这里在编译期判一次，能用就用，不能用就报错，而不是
// 等运行时崩。确知目标支持未对齐访问时可以定义 META_IOSTREAM_ALLOW_UNALIGNED=1。
#ifndef META_IOSTREAM_ALLOW_UNALIGNED
#  if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
      defined(_M_IX86) || defined(__aarch64__) || defined(_M_ARM64) || \
      defined(__ARM_FEATURE_UNALIGNED) || defined(__powerpc__) || \
      defined(__s390x__)
#    define META_IOSTREAM_ALLOW_UNALIGNED 1
#  else
#    define META_IOSTREAM_ALLOW_UNALIGNED 0
#  endif
#endif

// 目标 CPU/ABI 是否能安全地读写未对齐对象。
inline constexpr bool unaligned_access_ok = META_IOSTREAM_ALLOW_UNALIGNED != 0;

// 这个对齐边界能否保证 T 的每一格都对齐（alignment==0 表示按 alignof(T)）。
// 注意判据是 alignof(T) | alignment：alignment=4、T=double 仍然可能未对齐。
template <class T, std::size_t alignment>
inline constexpr bool layout_is_aligned =
    alignment == 0 || alignment % alignof(T) == 0;

namespace aligned_iterator_detail {
// alignment == 0（默认）：按类型自己的 alignof 对齐；
// alignment != 0：必须是 2 的幂，用它作为对齐边界（1 = 紧凑排列）。
template <std::size_t start, class from_ins, std::size_t alignment = 0>
struct seek_to {
  static_assert(alignment == 0 || std::has_single_bit(alignment),
                "alignment must be 0 (auto = alignof(T)) or a power of two");

  using type = from_ins;
  static constexpr std::size_t align =
      alignment == 0 ? alignof(from_ins) : alignment;

  // 每一格都对齐（或目标允许未对齐访问）才允许真正解引用。
  static constexpr bool access_is_aligned =
      layout_is_aligned<from_ins, alignment> || unaligned_access_ok;

  // 向上取整到 align 的倍数：start + ((-start) & (align - 1))。
  // 与 (start + align - 1) & ~(align - 1) 完全等价，但只用「取负 + 与 + 加」，
  // 既不会在 start 接近 SIZE_MAX 时溢出，编译期求值代价也一样低
  //（没有除法/取模，都是常量折叠）。
  static constexpr std::size_t value = start + ((0 - start) & (align - 1));

  // 下一格的起点。字节偏移是「布局事实」，与目标字节序无关
  //（大端/小端只决定一个多字节值内部各字节的顺序，不改变字段占用的区间），
  // 所以 advance 这里就是一个纯粹的加法；字节序的处理发生在值编解码处，
  // 不在偏移递推里。
  using advance_t = std::integral_constant<std::size_t, value + sizeof(from_ins)>;

 private:
  // 槽位指针：const 性跟着传进来的 base 走（const std::byte* → const T*）。
  // 用 if constexpr 而不是 std::conditional_t：只有被选中的分支才会实例化。
  // 所有访问都经过这一个地方，偏移运算只写一次。
  template <class Byte>
    requires std::is_same_v<std::remove_cv_t<Byte>, std::byte>
  static auto* slot(Byte* base_ptr) noexcept {
    if constexpr (std::is_const_v<Byte>) {
      return reinterpret_cast<type const*>(base_ptr + value);
    } else {
      return reinterpret_cast<type*>(base_ptr + value);
    }
  }

 public:
  void emplace(std::byte* base_ptr, type const& val) const {
    static_assert(access_is_aligned,
                  "seek_to: packed layout on a target without unaligned access "
                  "(use meta_wire_iterator, or define META_IOSTREAM_ALLOW_UNALIGNED=1)");
    ::new (static_cast<void*>(slot(base_ptr))) type{val};
  }
  void emplace(std::byte* base_ptr, type&& val) const {
    static_assert(access_is_aligned,
                  "seek_to: packed layout on a target without unaligned access "
                  "(use meta_wire_iterator, or define META_IOSTREAM_ALLOW_UNALIGNED=1)");
    ::new (static_cast<void*>(slot(base_ptr))) type{std::move(val)};
  }
  void destroy(std::byte* base_ptr) const {
    static_assert(access_is_aligned,
                  "seek_to: packed layout on a target without unaligned access "
                  "(use meta_wire_iterator, or define META_IOSTREAM_ALLOW_UNALIGNED=1)");
    std::destroy_at(slot(base_ptr));
  }

  std::byte* address_of(std::byte* base_ptr) const noexcept {
    return base_ptr + value;
  }

  // 取槽位上已构造对象的引用：传 const 的 base 就得 const T&，否则得 T&。
  // 合并了原来的 get / c_get（不再需要 c_ 版本），返回的始终是引用；
  // launder 用来告诉编译器「这个地址上的对象是 placement new 造出来的」。
  template <class Byte>
    requires std::is_same_v<std::remove_cv_t<Byte>, std::byte>
  auto& get(Byte* base_ptr) const noexcept {
    static_assert(access_is_aligned,
                  "seek_to: packed layout on a target without unaligned access "
                  "(use meta_wire_iterator, or define META_IOSTREAM_ALLOW_UNALIGNED=1)");
    return *std::launder(slot(base_ptr));
  }
};

// 推进器：alignment 决定下一格的对齐边界（0 = 用类型自己的 alignof）。
// 它仍然只是普通元对象的函数部分——apply / initialize 两个别名，
// 所以 meta_object<advance_f<A>> 照旧能放进 meta_states::run / meta_looper / meta_pipe。
template <std::size_t alignment = 0>
struct advance_f {
  static_assert(alignment == 0 || std::has_single_bit(alignment),
                "meta_aligned_iterator alignment must be 0 or a power of two");

  template <class this_seek, class from_is>
  using apply = seek_to<this_seek::advance_t::value, from_is, alignment>;

  // 初始化：只吃真正的类型。必须用 !no_arguments 挡掉"无参哨兵" ——
  // 否则 initialize 会被判定成"可无参初始化"，把哨兵当成第一个元素类型。
  template <class from_is>
    requires(!no_arguments<from_is>)
  using initialize = seek_to<0, from_is, alignment>;
};
}  // namespace aligned_iterator_detail

// 把类型流映射成"对齐后的字节偏移"：状态就是个 seek_to，
// 运行期可以用 emplace / get / destroy / address_of 在那个偏移上干活。
// 对齐边界：默认 0 = 每个类型按自己的 alignof 对齐；给定 2 的幂（1/2/4/…）
// 则所有槽位统一用这个边界。
// 用法只有一种：调用元对象本身。想要"某一格上的迭代器"就 meta_set：
//   meta_invoke<meta_aligned_iterator<>, T>::type          → 第一格（seek_to<0, T>）
//   meta_invoke<meta_aligned_iterator<>::meta_set<back>, T> → 接着 back 往下推进
template <std::size_t alignment = 0>
using meta_aligned_iterator =
    meta_object<aligned_iterator_detail::advance_f<alignment>>;

// ============================================================================
// 6) 线上字节布局：meta_wire_iterator（packed + 指定端序）
// ============================================================================
//
// 和 meta_aligned_iterator 共用同一套偏移递推：偏移只跟协议有关，与机器端序
// 无关（大端/小端只决定一个多字节字段内部各字节的顺序，不改变字段占哪几个
// 字节）。两者的区别只在“值怎么进出”：
//   - aligned 版把缓冲区当成“已经躺着一个本机表示的 T”：emplace / get 返回引用；
//   - wire 版把缓冲区当成“纯字节”：read / write 按值 + memcpy，不构造对象，
//     并按协议端序解码/编码；要不要翻转由 std::endian::native 在编译期决定。
//
// 所以这里不需要“根据机器调整布局策略”：布局按协议（packed）走，机器只影响
// 读写的 bswap。
namespace wire_iterator_detail {

// 端序 codec：协议端序是模板参数（协议事实），机器端序是编译期常量
// （机器事实），两者不同才翻转。所有访问都走 memcpy —— packed 偏移不保证
// alignof(T)，reinterpret_cast<T*> 是 UB；缓冲区是借来的字节，也不能
// placement new。
template <std::endian Wire>
struct wire_codec {
  static_assert(std::endian::native == std::endian::little ||
                    std::endian::native == std::endian::big,
                "native byte order is neither little nor big "
                "(no single byte-swap policy)");

  static constexpr bool swap = std::endian::native != Wire;

  // 翻转一个值：整型直接 byteswap；浮点按位模式走 byteswap；
  // 枚举按底层类型走。单字节/数组/聚合不进这里（协议里的字节串不翻）。
  template <class T>
  static constexpr T swapped(T v) noexcept {
    if constexpr (std::is_enum_v<T>) {
      using underlying = std::underlying_type_t<T>;
      return static_cast<T>(std::byteswap(static_cast<underlying>(v)));
    } else if constexpr (std::is_floating_point_v<T>) {
      using bits_t =
          std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
      return std::bit_cast<T>(std::byteswap(std::bit_cast<bits_t>(v)));
    } else {
      return std::byteswap(v);
    }
  }

  // 按字节拷进/拷出。std::memcpy 不是 constexpr，这里用 std::bit_cast +
  // 字节循环：常量求值和运行期都能用，编译器照旧会把定长循环折成一次访存。
  template <class T>
  static constexpr T from_bytes(std::byte const* p) noexcept {
    std::array<std::byte, sizeof(T)> bytes{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes[i] = p[i];
    }
    return std::bit_cast<T>(bytes);
  }

  template <class T>
  static constexpr void to_bytes(std::byte* p, T v) noexcept {
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(v);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      p[i] = bytes[i];
    }
  }

  template <class T>
  static constexpr T read(std::byte const* p) noexcept {
    static_assert(!std::is_const_v<T>, "wire_codec::read needs a non-const T");
    static_assert(std::is_trivially_copyable_v<T>,
                  "wire_codec: only trivially copyable fields can be decoded");
    T v = from_bytes<T>(p);
    if constexpr (swap && sizeof(T) > 1 && !std::is_array_v<T>) {
      if constexpr (std::is_integral_v<T> || std::is_enum_v<T> ||
                    std::is_floating_point_v<T>) {
        v = swapped(v);
      }
    }
    return v;
  }

  template <class T>
  static constexpr void write(std::byte* p, T v) noexcept {
    static_assert(std::is_trivially_copyable_v<T>,
                  "wire_codec: only trivially copyable fields can be encoded");
    if constexpr (swap && sizeof(T) > 1 && !std::is_array_v<T>) {
      if constexpr (std::is_integral_v<T> || std::is_enum_v<T> ||
                    std::is_floating_point_v<T>) {
        v = swapped(v);
      }
    }
    to_bytes<T>(p, v);
  }
};

// 一格：偏移 + 端序。advance_t 仍然只是加法（与 aligned 版完全一致），
// 端序不影响偏移递推。
template <std::size_t start, class from_ins, std::size_t alignment = 1,
          std::endian Wire = std::endian::big>
struct wire_seek_to {
  static_assert(alignment == 0 || std::has_single_bit(alignment),
                "alignment must be 0 (auto = alignof(T)) or a power of two");

  using type = from_ins;
  using codec = wire_codec<Wire>;
  static constexpr std::endian order = Wire;
  static constexpr std::size_t align =
      alignment == 0 ? alignof(from_ins) : alignment;
  // 与 seek_to 同一公式：向上取整到 align 的倍数。
  static constexpr std::size_t value = start + ((0 - start) & (align - 1));
  using advance_t =
      std::integral_constant<std::size_t, value + sizeof(from_ins)>;

  static constexpr std::byte* slot(std::byte* base) noexcept {
    return base + value;
  }
  static constexpr std::byte const* slot(std::byte const* base) noexcept {
    return base + value;
  }

  constexpr std::byte* address_of(std::byte* base) const noexcept {
    return slot(base);
  }
  constexpr std::byte const* address_of(std::byte const* base) const noexcept {
    return slot(base);
  }

  // 读/写的类型就是这一格的字段类型（type = from_ins），不需要也不应该再指定：
  // 偏移是按这一格的 sizeof 推进的，按别的类型读等于越界重解释。
  constexpr std::remove_cv_t<type> read(std::byte const* base) const noexcept {
    return codec::template read<std::remove_cv_t<type>>(slot(base));
  }

  constexpr void write(std::byte* base, type v) const noexcept
    requires(!std::is_const_v<type>)
  {
    codec::template write<type>(slot(base), v);
  }
};

// 推进器：与 advance_f 同形，只是每格换成 wire_seek_to。
template <std::size_t alignment = 1, std::endian Wire = std::endian::big>
struct wire_advance_f {
  static_assert(alignment == 0 || std::has_single_bit(alignment),
                "meta_wire_iterator alignment must be 0 or a power of two");

  template <class this_seek, class from_is>
  using apply =
      wire_seek_to<this_seek::advance_t::value, from_is, alignment, Wire>;

  // 初始化：只吃真正的类型（挡掉无参哨兵），与 advance_f 一致。
  template <class from_is>
    requires(!no_arguments<from_is>)
  using initialize = wire_seek_to<0, from_is, alignment, Wire>;
};
}  // namespace wire_iterator_detail

// 把类型流映射成“线上字节偏移 + 端序解码”：状态是个 wire_seek_to，
// 运行期用 read / write / address_of 在那个偏移上按协议端序搬运字节。
// 布局默认 packed（alignment = 1），端序默认大端；两者都是**协议**参数，
// 机器端序只影响 read/write 内部要不要 byteswap，不影响偏移。
//   meta_invoke<meta_wire_iterator<>, T>::type → 第一格（wire_seek_to<0, T>）
//   再 meta_invoke 下去                          → 接着往后推进
template <std::size_t alignment = 1, std::endian Wire = std::endian::big>
using meta_wire_iterator =
    meta_object<wire_iterator_detail::wire_advance_f<alignment, Wire>>;

}  // namespace meta_iostream
