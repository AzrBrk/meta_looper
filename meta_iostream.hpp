#pragma once

// ============================================================================
// meta_iostream: the i/ostream collection, independent of meta_stream
// (the part extracted from meta_ios: plain meta objects, no meta_stream dependency).
//
// Every stream only implements the meta-object protocol:
//   istream: ::type (remaining state) / ::ret (the value emitted now) / apply (advance)
//   ostream: ::type (accumulated result) / apply(this_state, from_ins) (absorb one input)
// so they plug straight into meta_states::run / meta_states::looper and are
// interchangeable with hand-written streams.
//
// ---------------------------------------------------------------------------
// meta_istream semantics (initializer form): construction does not touch the
// user type list. The first call runs initialize, which copies that list into
// an exp_list payload; every later apply advances only on that payload
// (pop_front / front). Therefore:
//   - the user type list is never modified;
//   - any "type-list shaped" input works (exp_list, std::tuple, std::variant);
//   - the stream starts uninitialized and joins the looper's no-argument
//     initialization protocol.
// ---------------------------------------------------------------------------
//
// meta_ios.hpp aggregates the final API; this component layer depends on
// neither the pipe nor the API facade.
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

// ===== Common aliases (this layer only uses the meta-object protocol and the
// ===== meta_utilities primitives) =====
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

// The payload declares "I am done": the looper and conditions use it to detect
// the end of a stream instead of guessing the payload shape (list length,
// counter, ...).
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
// 1) Basic istream / ostream
// ============================================================================

namespace basic_istream_detail {
// Advance function with an initializer: initialize builds the payload (a copy of
// the user list as an exp_list), and every later step only pops from that
// payload - the user type list is never touched.
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
// Iterates the payload: rebinds the user list's elements onto is_list and
// borrows only its length (exp_size).
  template <class ...Tys>
  struct is_list{};

// Explicit terminal state: the old implementation let the reverse `next` count
// down from 0 to size_t(-1) and tested I == static_cast<std::size_t>(-1) - the
// index was "wrapped out" of arithmetic, and is_list_iter<-1> still held an
// illegal `type = exp_select<-1, TL>`. Now "ended" is a state of its own: no
// index, no out-of-range selection, advancing further stays put.
  template<class TL>
  struct is_list_end: end_of_stream<true>{
  static constexpr std::size_t length = 0;  // no remaining elements when ended
    using type = no_exist_type;
  using next = is_list_end;     // absorbing state
  using reverse = is_list_end;  // the terminal state has no position either way
  };

  template<std::size_t I, bool direction, class TL>
  struct is_list_iter;

// Forward and reverse differ only at the boundary cell: forward ends after
// size-1 and reverse after 0. An explicit split replaces conditional_t: with
// Ended == true only the specialization is instantiated, so I - 1 / I + 1 never
// even form and size_t(-1) cannot appear in a type argument.
  template<bool Ended, std::size_t I, bool direction, class TL>
  struct next_iter{
    using type = is_list_iter<(direction ? I + 1 : I - 1), direction, TL>;
  };
  template<std::size_t I, bool direction, class TL>
  struct next_iter<true, I, direction, TL>{
    using type = is_list_end<TL>;
  };

// I is an absolute index: forward walks 0 -> total-1, reverse total-1 -> 0.
// Both directions share the same index semantics; only the end condition of
// next differs.
// length follows the exp_size protocol: exp_size checks for ::length first (the
// has_length branch in meta_utilities) and uses it directly. It reports "how many
// are left including the current element", matching meta_istream (pop_front, the
// payload length is the remaining count), so states.left() decreases in both
// directions as the stream advances.
  template<std::size_t I, bool direction, class TL>
  struct is_list_iter: end_of_stream<false>{
    static constexpr std::size_t total = exp_size<TL>;
    static constexpr bool next_ends = direction ? (I + 1 == total) : (I == 0);
    static constexpr std::size_t length = direction ? (total - I) : (I + 1);
    using type = exp_utilities::exp_select_from<0, I, TL>;
  using reverse = is_list_iter<I, !direction, TL>;  // same position, flipped direction
    using next = typename next_iter<next_ends, I, direction, TL>::type;
  };

  template<class TL, bool direction = true>
  struct list_iter_advance_f{
    using payload_t = reload_to<TL, is_list<>>;

// An empty list starts in the terminal state; otherwise forward starts at 0 and
// reverse at size-1.
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

template<class TL>
struct ostream_f {
  template<class From> requires (!no_arguments<From>)
  using initialize = add_f<TL, From>;
  template<class Current, class From>
  using apply = add_f<Current, From>;
};
}  // namespace basic_ostream_detail

template <class TL = exp_list<>>
using meta_ostream = meta_object<basic_ostream_detail::ostream_f<TL>>;

// ============================================================================
// 2) Transforms
// ============================================================================

namespace transform_istream_detail {
using basic_istream_detail::list_dec;
using basic_istream_detail::front_of;

// Take the front of current, then run the user F over it.
template <class F, class this_list>
using pop_transform_f = meta_invoke<F, front_of<this_list>>;
}  // namespace transform_istream_detail

template <class TL, class F>
using meta_transform_istream =
    meta_ret_object<transform_istream_detail::list_dec<TL>,
                    bind_binary<transform_istream_detail::pop_transform_f, F>>;

namespace transform_ostream_detail {
using basic_ostream_detail::add_f;

// On absorb, run F(this_list, from_ins) first, then push the result.
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
// 3) Collecting ostreams
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

// The incoming value is itself a type list: flatten it and merge element by
// element rather than treating the list as one element.
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
// add_front: insert T at the front of any "type-list shaped" container, keeping
// its own template (never coerced into exp_list). It is basic_ostream's add,
// but inserting at the front.
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

// Every step inserts at the front, so the final list is in reverse arrival order
// (handy for reverse-order destruction).
struct reverse_collect_f {
  template <class this_list, class from_ins>
  using apply = add_front_f<this_list, from_ins>;
};
}  // namespace reverse_ostream_detail

template <class TL = exp_list<>>
using meta_rostream = meta_object<TL, reverse_ostream_detail::reverse_collect_f>;

namespace filter_ostream_detail {
using basic_ostream_detail::add_f;

// Both branches are thunks, so add_f is never instantiated when the predicate is
// false.
struct add_pair_fn {
  template <class this_list, class from_ins>
  using apply = add_f<this_list, from_ins>;
};
struct keep_list_fn {
  template <class this_list, class from_ins>
  using apply = this_list;
};

// Accept only when the predicate holds; otherwise the list stays unchanged this
// step.
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

// The state is replaced by whatever came in each step (an ostream with
// replaceable state).
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

// mo::meta_set<T> expands only when con is true.
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
// Index by the current list length: F is applied to the type at that position
// and to the incoming argument.
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
// Some streams never drain themselves; once the length reaches L, F is no longer
// applied (avoids out-of-range selection).
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
// 4) Generated istreams (index / repeat)
// ============================================================================

namespace index_istream_detail {
using exp_utilities::Idx;
using exp_utilities::inc_idx_t;

// The payload is a one-element exp_list: exp_size == 1, so the stream never ends
// because of emptiness.
template <std::size_t start>
using my_counter = exp_list<Idx<start>>;

template <class this_list>
using make_inc_t = exp_list<inc_idx_t<typename this_list::template at<0>>>;

template <class this_list>
using ret_index = typename this_list::template at<0>;
}  // namespace index_istream_detail

// Never ends: emits an increasing index every time.
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
// The emitted position is normalized to std::integral_constant (not the state
// type itself) so exact checks such as is_same<cache, integral_constant<size_t,
// K>> hit; the loop state stays index_counter.
  template <class this_index, class...>
  using apply = std::integral_constant<std::size_t, Len - this_index::value>;
};
}  // namespace index_sequence_istream_detail

// Bounded: emits 0, 1, ..., Len-1, then ends naturally through end_of_stream.
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

// Never ends: emits the same T every time.
template <class T>
using meta_repeat_istream =
    meta_ret_object<exp_list<T>, unary<repeat_istream_detail::do_nothing>,
                    unary<repeat_istream_detail::ret_self>>;

// ============================================================================
// 5) Runtime byte layout: meta_aligned_iterator
//
// Weakly aligned CPUs (old ARM, MIPS, SPARC, RISC-V with -mstrict-align, ...)
// cannot access unaligned objects: a layout with alignment < alignof(T) (packed
// = 1, typically) is UB there and can fault. The check happens at compile time:
// use the layout when allowed, error out otherwise, instead of crashing at run
// time. Define META_IOSTREAM_ALLOW_UNALIGNED=1 when the target is known to
// accept unaligned access.
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

// Whether the target CPU/ABI can safely read and write unaligned objects.
inline constexpr bool unaligned_access_ok = META_IOSTREAM_ALLOW_UNALIGNED != 0;

// Whether this alignment boundary keeps every cell of T aligned (alignment == 0
// means "use alignof(T)").
// The test is alignof(T) | alignment: alignment = 4 with T = double can still be
// unaligned.
template <class T, std::size_t alignment>
inline constexpr bool layout_is_aligned =
    alignment == 0 || alignment % alignof(T) == 0;

namespace aligned_iterator_detail {
// alignment == 0 (default): align by the type's own alignof.
// alignment != 0: must be a power of two and is used as the boundary (1 = packed).
template <std::size_t start, class from_ins, std::size_t alignment = 0>
struct seek_to {
  static_assert(alignment == 0 || std::has_single_bit(alignment),
                "alignment must be 0 (auto = alignof(T)) or a power of two");

  using type = from_ins;
  static constexpr std::size_t align =
      alignment == 0 ? alignof(from_ins) : alignment;

// Dereferencing is allowed only when every cell is aligned, or the target permits
// unaligned access.
  static constexpr bool access_is_aligned =
      layout_is_aligned<from_ins, alignment> || unaligned_access_ok;

// Round up to a multiple of align: start + ((-start) & (align - 1)).
// Equivalent to (start + align - 1) & ~(align - 1) but uses only negate + AND +
// add: no overflow when start is near SIZE_MAX, and the constant-evaluation cost
// is the same (no division or modulo).
  static constexpr std::size_t value = start + ((0 - start) & (align - 1));

// Start of the next cell. Byte offsets are a layout fact, independent of the
// target byte order (endianness only orders the bytes inside one multi-byte
// value; the occupied range does not change), so advance here is pure addition.
// Byte order is handled where values are encoded and decoded, not in the offset
// recurrence.
  using advance_t = std::integral_constant<std::size_t, value + sizeof(from_ins)>;

 private:
// Slot pointer: constness follows the incoming base (const std::byte* -> const
// T*). if constexpr instead of std::conditional_t: only the selected branch is
// instantiated. Every access goes through this one place, so the offset
// arithmetic is written once.
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

// Reference to the object already constructed in the slot: a const base yields
// const T&, otherwise T&. This subsumes the old get / c_get pair (no c_ variant
// any more) and always returns a reference; std::launder yields the pointer to
// the current object when needed. It constructs nothing and starts no lifetime.
  template <class Byte>
    requires std::is_same_v<std::remove_cv_t<Byte>, std::byte>
  auto& get(Byte* base_ptr) const noexcept {
    static_assert(access_is_aligned,
                  "seek_to: packed layout on a target without unaligned access "
                  "(use meta_wire_iterator, or define META_IOSTREAM_ALLOW_UNALIGNED=1)");
    return *std::launder(slot(base_ptr));
  }

};

// Advancer: alignment sets the boundary of the next cell (0 = the type's own
// alignof). It is still only the function part of an ordinary meta object (the
// apply / initialize aliases), so meta_object<advance_f<A>> still fits
// meta_states::run / meta_looper / meta_pipe.
template <std::size_t alignment = 0>
struct advance_f {
  static_assert(alignment == 0 || std::has_single_bit(alignment),
                "meta_aligned_iterator alignment must be 0 or a power of two");

  template <class this_seek, class from_is>
  using apply = seek_to<this_seek::advance_t::value, from_is, alignment>;

// Initialization accepts real types only. The !no_arguments constraint must
// reject the no-argument sentinel, or initialize would look "no-argument
// initializable" and the sentinel would become the first element type.
  template <class from_is>
    requires(!no_arguments<from_is>)
  using initialize = seek_to<0, from_is, alignment>;
};
}  // namespace aligned_iterator_detail

// Maps a type stream to aligned byte offsets: the state is a seek_to and run
// time can emplace / get / destroy / address_of at that offset. Alignment
// boundary: 0 (default) aligns every type by its own alignof; a power of two
// (1/2/4/...) uses that boundary for every slot. There is one usage - invoke the
// meta object itself; for an iterator at a specific cell use meta_set:
//   meta_invoke<meta_aligned_iterator<>, T>::type          -> first cell (seek_to<0, T>)
//   meta_invoke<meta_aligned_iterator<>::meta_set<back>, T> -> continue after back
template <std::size_t alignment = 0>
using meta_aligned_iterator =
    meta_object<aligned_iterator_detail::advance_f<alignment>>;

// ============================================================================
// 6) Wire byte layout: meta_wire_iterator (packed + explicit byte order)
//
// Shares the offset recurrence with meta_aligned_iterator: offsets depend only on
// the protocol, not on machine endianness (endianness orders the bytes inside a
// multi-byte field; it does not change which bytes the field occupies). The two
// differ only in how values go in and out:
//   - aligned treats the buffer as "a T already stored in native form":
//     emplace / get return references;
//   - wire treats the buffer as raw bytes: read / write by value + memcpy, no
//     object construction, decoding/encoding in protocol byte order, with the
//     flip decided at compile time from std::endian::native.
// So nothing here adjusts the layout to the machine: the layout follows the
// protocol (packed) and the machine only affects the bswap inside read/write.
namespace wire_iterator_detail {

// Endianness codec: the protocol byte order is a template argument (a protocol
// fact) and the machine byte order is a compile-time constant (a machine fact);
// a flip happens only when they differ. All access goes through memcpy - a
// packed offset does not guarantee alignof(T), so reinterpret_cast<T*> is UB.
// placement new。
template <std::endian Wire>
struct wire_codec {
  static_assert(std::endian::native == std::endian::little ||
                    std::endian::native == std::endian::big,
                "native byte order is neither little nor big "
                "(no single byte-swap policy)");

  static constexpr bool swap = std::endian::native != Wire;

// Flip a value: integers byteswap directly, floating point byteswaps its bit
// pattern, enums follow their underlying type. Single bytes, arrays and
// aggregates do not come through here (protocol byte strings are not flipped).
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

// Copy bytes in and out. std::memcpy is not constexpr, so this uses std::bit_cast
// plus a byte loop: it works at compile time and at run time, and the compiler
// still folds the fixed-size loop into one access.
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

// One cell: offset + byte order. advance_t is still just an addition (identical
// to the aligned version); byte order never affects the offset recurrence.
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
// Same formula as seek_to: round up to a multiple of align.
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

// The type read or written is exactly this cell's field type (type = from_ins)
// and must not be given again: the offset advances by this cell's sizeof, so
// reading the cell as another type is an out-of-bounds reinterpretation.
  constexpr std::remove_cv_t<type> read(std::byte const* base) const noexcept {
    return codec::template read<std::remove_cv_t<type>>(slot(base));
  }

  constexpr void write(std::byte* base, type v) const noexcept
    requires(!std::is_const_v<type>)
  {
    codec::template write<type>(slot(base), v);
  }
};

// Advancer: same shape as advance_f, with wire_seek_to per cell.
template <std::size_t alignment = 1, std::endian Wire = std::endian::big>
struct wire_advance_f {
  static_assert(alignment == 0 || std::has_single_bit(alignment),
                "meta_wire_iterator alignment must be 0 or a power of two");

  template <class this_seek, class from_is>
  using apply =
      wire_seek_to<this_seek::advance_t::value, from_is, alignment, Wire>;

// Initialization accepts real types only (rejects the no-argument sentinel), as
// in advance_f.
  template <class from_is>
    requires(!no_arguments<from_is>)
  using initialize = wire_seek_to<0, from_is, alignment, Wire>;
};
}  // namespace wire_iterator_detail

// Maps a type stream to "wire byte offset + byte-order decoding": the state is a
// wire_seek_to and run time uses read / write / address_of to move bytes at that
// offset in protocol byte order. The layout is packed by default
// (alignment = 1) and the byte order is big endian by default; both are protocol
// parameters, and machine endianness only decides whether read/write byteswaps,
// never the offset.
//   meta_invoke<meta_wire_iterator<>, T>::type -> first cell (wire_seek_to<0, T>)
//   further meta_invoke calls                  -> keep advancing
template <std::size_t alignment = 1, std::endian Wire = std::endian::big>
using meta_wire_iterator =
    meta_object<wire_iterator_detail::wire_advance_f<alignment, Wire>>;

}  // namespace meta_iostream
