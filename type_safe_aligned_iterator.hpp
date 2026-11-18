#pragma once

// ============================================================================
// type_safe_aligned_iterator: type-safe aligned byte-buffer iterators.
//
// Ported from type_safe_aligned_iterator.hpp (meta_stream_head_only) onto meta_looper.
// The public API is unchanged:
//
//   type_safe_aligned_iterator<Alignment, Ty...>         forward iterator (operator* / ++ / ==)
//   type_safe_aligned_reverse_iterator<Alignment, Ty...> reverse iterator (used by destroy)
//   type_safe_aligned_range<Alignment, Ty...>            begin/end/rbegin/rend adapter
//   aligned_iterator_storage<Ty...>                     checked proxy for a single slot
//   initialize(range, args...)                          constructs every element by layout
//   field_size<Alignment, Ty...>()                      byte size of the whole layout
//
// Alignment: 0 = each cell aligned by alignof (memory layout, the default); a given
// power of two uses that boundary for every slot (1 = packed, for wire/file formats).
//
// References: Ty... may spell T& / T const& directly; layout, masks and streams all use
// stored_type_t<Ty> (std::reference_wrapper). Nothing is wrapped at the value level:
// construction and assignment still require std::ref / std::cref.
//
// The implementation is built entirely on meta_looper:
//
//   - Aligned offsets: meta_pipe feeds the type stream through meta_aligned_iterator<>;
//     forward collects absolute offsets with meta_ostream<>, reverse uses meta_rostream<>
//     to collect a reversed table of absolute coordinates (offsets still count from the
//     buffer start; only the order is reversed). Each iterator owns its own table, and a
//     slot index is that table subscript - no cross-table arithmetic.
//   - Total size: meta_pipe runs meta_aligned_iterator<> to the end and reads the last
//     seek_to's advance_t; max alignment is a plain fold (max_alignof_v).
//   - type_mask: a direct pack fold over const_convertible, packed into two u64 halves
//     (low = bits 0..63, high = 64..127). The old 128-bit u128 MSO bitmap is retired (D1);
//     the two-word shape keeps the 128-type limit with no u128 arithmetic.
// ============================================================================

#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <istream>
#include <limits>
#include <new>
#include <ostream>
#include <tuple>
#include <type_traits>
#include <utility>

#include "meta_ios.hpp"
#include "meta_stream.hpp"
#include "meta_stream_control.hpp"

namespace type_safe_aligned {

using namespace exp_utilities;
using namespace meta_iostream;
using namespace meta_objects;
using namespace meta_states;
using namespace meta_ios;

// ============================================================================
// 0) Reference normalization
// ============================================================================
// A reference spelled at the template level becomes std::reference_wrapper:
//   Ty&         -> std::reference_wrapper<Ty>
//   Ty const&   -> std::reference_wrapper<Ty const>
// Only the type is converted, never a value: construction and assignment still require
// the user to write std::ref / std::cref, so "this cell holds a reference" stays
// explicit. For non-reference types the mapping is the identity, hence idempotent.
template <class T>
using stored_type_t =
    std::conditional_t<std::is_reference_v<T>,
                       std::reference_wrapper<std::remove_reference_t<T>>, T>;

// ============================================================================
// 1) Layout computation
// ============================================================================

// Strictest alignment across the (stored) types: field_size rounds by this when
// Alignment == 0. A plain fold; the old larger_alignment_calculator looper is
// gone because max-align is a value, not a type (no looper needed to fold it).
template <class... Ty>
constexpr std::size_t max_alignof_v = [] {
  std::size_t result = 1;
  ((result = alignof(Ty) > result ? alignof(Ty) : result), ...);
  return result;
}();

namespace layout_details {

// The aligned iterator after consuming every type: its ::type is the last
// seek_to, whose advance_t is the end offset (before the tail rounding). The
// old looper spelling is gone; the pipe is the receive-to-init walker now.
template <std::size_t Alignment, class... Ty>
using aligned_receiver_after_all =
    typename meta_pipe<meta_istream_list<Ty...>>
        ::template run<meta_aligned_iterator<Alignment>>::type::to;

}  // namespace layout_details

// Byte size of the whole layout: advance past the last slot, then round by the layout boundary.
// Alignment == 0 rounds by the strictest type alignment (memory layout, tail padding included);
// Alignment != 0 rounds by that boundary (1 = packed, no tail padding).
// References are normalized to std::reference_wrapper first (pointer size and alignment).
template <std::size_t Alignment = 0, class... Ty>
consteval std::size_t field_size() {
  static_assert(sizeof...(Ty) > 0, "field_size requires at least one type");
  static_assert(Alignment == 0 || std::has_single_bit(Alignment),
                "field_size alignment must be 0 or a power of two");
  using end_seek = layout_details::aligned_receiver_after_all<
      Alignment, stored_type_t<Ty>...>;
  constexpr std::size_t raw_end = end_seek::type::advance_t::value;
  constexpr std::size_t align =
      Alignment == 0 ? max_alignof_v<stored_type_t<Ty>...> : Alignment;
  return raw_end + ((0 - raw_end) & (align - 1));
}

// ============================================================================
// 2) type_mask: the "slot usable as T" bitmap, as two u64 words
// ============================================================================
//
// Bit i is 1 iff Ty[i] is usable as T (identical, or equal after adding const).
// The bitmap used to be a 128-bit u128 accumulated through an MSO looper; u128
// is retired (D1), so it is now two plain u64 words built directly over the
// pack. low holds bits 0..63, high bits 64..127, which keeps the 128-type limit
// without any u128 arithmetic.

namespace mask_details {

// Whether type U on a slot is usable as T: only the "add const" direction is allowed.
//   U = int,       T = int        -> yes (identical)
//   U = int,       T = const int  -> yes (& to const& qualification conversion)
//   U = const int, T = int        -> no (would drop const; the reverse is rejected)
// A reference itself has no cv; const applies to the referred type, so cv is judged after remove_reference.
template <class T, class U>
inline constexpr bool const_convertible =
    std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<U>> &&
    (std::is_const_v<std::remove_reference_t<T>> ||
     !std::is_const_v<std::remove_reference_t<U>>);

// The 128-bit match bitmap as two u64 halves (u128 retired; D1).
struct type_mask {
  std::uint64_t low = 0;
  std::uint64_t high = 0;
};

}  // namespace mask_details

// Bit i is 1 iff Ty[i] is usable as T: identical, or equal after adding const to Ty[i].
// Adding const matches (& -> const&); removing const does not.
// T may repeat in the list; the limit is 128 types (low = bits 0..63, high = 64..127).
template <class T, class... Ty>
inline constexpr mask_details::type_mask type_mask_series = [] {
  static_assert(sizeof...(Ty) <= 128,
                "type_safe_aligned_range supports at most 128 types");
  mask_details::type_mask m{};
  constexpr bool hits[] = {mask_details::const_convertible<T, Ty>...};
  for (std::size_t i = 0; i < sizeof...(Ty); ++i) {
    if (!hits[i]) continue;
    if (i < 64) m.low |= (std::uint64_t{1} << i);
    else m.high |= (std::uint64_t{1} << (i - 64));
  }
  return m;
}();

// The "member of the pack" test, with the same criterion as type_mask_series:
// can T be read from a slot of Ty... (adding const is fine, removing const is not).
template <class T, class... Ty>
inline constexpr bool is_slot_convertible =
    (mask_details::const_convertible<T, Ty> || ...);

// ============================================================================
// 3.0) Run-time numeric operand
// ============================================================================
// The proxy is heterogeneous: whether "slot + ?" is int or double is only known at run time,
// so operands must travel one uniform channel. A floating-point overload family per operator
// would double the proxy overload set and instantiate one more thunk per slot type; a small
// operand type keeps operators non-templated on one operation-table signature, and new
// operand kinds only add fields here.
//
// The channel carries exactly two interpretations:
//   - std::uint64_t: exact integers, preserving the original semantics (no double detour)
//   - double      : floating-point increments
// Other arithmetic types (int, std::uint32_t, float, ...) do not satisfy numeric_operand and
// have no viable overload; construction is explicit, so no accidental conversions happen.
struct operand {
  union {
    std::uint64_t u;
    double d;
  };
  bool is_float{false};

  constexpr explicit operand(std::uint64_t value) noexcept
      : u(value), is_float(false) {}
  constexpr explicit operand(double value) noexcept : d(value), is_float(true) {}

// Arithmetic uses double (matching the existing results); comparison uses long double, which
// represents both std::uint64_t and double exactly, so nothing is lost while comparing.
  constexpr double as_double() const noexcept {
    return is_float ? d : static_cast<double>(u);
  }
  constexpr long double as_long_double() const noexcept {
    return is_float ? static_cast<long double>(d)
                    : static_cast<long double>(u);
  }
};

// Operand constraint: exactly std::uint64_t or double. The proxy has operator T&(), and a
// non-template operator+=(operand) would need a user-defined conversion for the right operand,
// which makes Clang treat the built-in candidate (left operand through operator T&()) as
// equivalent and report ambiguity. Keeping one template per operator makes both sides identity
// conversions so the member wins, and the overload set does not grow.
template <class U>
concept numeric_operand =
    std::same_as<U, std::uint64_t> || std::same_as<U, double>;

// ============================================================================
// 3) Checked proxy for a single slot
// ============================================================================
// The criterion is "slot type U usable as T", allowing only the const-adding direction:
//   U == T                -> yes
//   U with const == T     -> yes (& to const& qualification conversion)
//   U without const == T  -> no (const& to & is rejected)
// References in Ty... are normalized to std::reference_wrapper (stored_type_t), so
// aligned_iterator_storage<int&> equals <std::reference_wrapper<int>>.
template <class... Ty>
struct aligned_iterator_storage {
// type_mask_series now uses all 128 flag bits; more than 128 types cannot be expressed.
// Such a list is rejected outright instead of silently producing a wrong safe().
  static_assert(sizeof...(Ty) <= 128,
                "aligned_iterator_storage: at most 128 types "
                "(type_mask_series is a 128-bit mask)");

  enum class error {
    null_pointer, type_not_safe, index_exceed,
    unsupported_operation, arithmetic_overflow, division_by_zero
  };

  class exception : public std::exception {
   public:
    explicit exception(error code) noexcept : code_(code) {}
    error code() const noexcept { return code_; }

    const char* what() const noexcept override {
      switch (code_) {
        case error::null_pointer:
          return "aligned_iterator_storage: null pointer";
        case error::type_not_safe:
          return "aligned_iterator_storage: type is not safe at this index";
        case error::index_exceed:
          return "aligned_iterator_storage: index exceeds type list";
        case error::unsupported_operation:
          return "aligned_iterator_storage: unsupported slot operation";
        case error::arithmetic_overflow:
          return "aligned_iterator_storage: arithmetic write exceeds slot range";
        case error::division_by_zero:
          return "aligned_iterator_storage: division by zero";
      }
      return "aligned_iterator_storage: unknown error";
    }

   private:
    error code_;
  };

  std::byte* ptr{nullptr};
  std::size_t m_type_index{0};

// Reject *it1 = *it2 (assigning one slot proxy to another): the implicit copy assignment
// would silently copy ptr/index (rebinding the iterator) instead of writing the value into the
// target slot. Write a value with *it = value; copy a proxy with an explicit copy ctor.
  aligned_iterator_storage& operator=(aligned_iterator_storage const&) = delete;

// The copy constructor is explicitly defaulted: operator[] returns the proxy by value and
// depends on it, and with copy assignment deleted Clang reports -Wdeprecated-copy on the
// implicit one. Declaring the copy constructor forces the default and two-argument aggregate
// constructors to be re-declared as well.
  aligned_iterator_storage() = default;
  aligned_iterator_storage(std::byte* base, std::size_t type_index) noexcept
      : ptr(base), m_type_index(type_index) {}
  aligned_iterator_storage(aligned_iterator_storage const&) = default;

// Whether T is allowed on this slot. T must be one of Ty... (or the const-qualified version);
// out-of-range indices and null pointers throw.
  template <class T>
  bool safe() const {
    if constexpr (!slot_type_allowed<T>()) {
      static_assert(slot_type_allowed<T>(),
                    "T must be one of aligned_iterator_storage's types "
                    "(or a const-qualified version of one)");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      return slot_mask_hit<T>();
    }
  }

// Starts the lifetime of T on the current slot with placement new.
  template <class T>
  bool emplace(T&& val) {
    using value_type = std::remove_cvref_t<T>;
    if constexpr (!slot_type_allowed<value_type>()) {
      static_assert(slot_type_allowed<value_type>(),
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!slot_mask_hit<value_type>()) {
        throw exception(error::type_not_safe);
      }
      ::new (static_cast<void*>(ptr)) value_type(std::forward<T>(val));
      return true;
    }
  }

// Checked T& on the current slot (adding const is allowed: an int slot reads as const int&).
  template <class T>
  operator T&() const {
    if constexpr (!slot_type_allowed<T>()) {
      static_assert(slot_type_allowed<T>(),
                    "T must be one of aligned_iterator_storage's types "
                    "(or a const-qualified version of one)");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!slot_mask_hit<T>()) {
        throw exception(error::type_not_safe);
      }
      return *reinterpret_cast<T*>(ptr);
    }
  }

// Checked assignment of a T already constructed on the current slot.
// Writes do not use the const-adding match (T must be the slot type itself, not its const version).
  template <class T>
  void operator=(T const& val) {
    using value_type = std::remove_cv_t<T>;
    if constexpr (!slot_type_allowed<value_type>()) {
      static_assert(slot_type_allowed<value_type>(),
                    "T must be one of aligned_iterator_storage's types");
    } else {
      if (m_type_index >= sizeof...(Ty)) {
        throw exception(error::index_exceed);
      }
      if (ptr == nullptr) {
        throw exception(error::null_pointer);
      }
      if (!slot_mask_hit<value_type>()) {
        throw exception(error::type_not_safe);
      }
      *reinterpret_cast<value_type*>(ptr) = val;
    }
  }

  template <class T>
  T& as() const {
    if (!safe<T>()) {
      throw exception(error::type_not_safe);
    }
    return *std::launder(reinterpret_cast<std::remove_reference_t<T>*>(ptr));
  }

  template <numeric_operand U>
  double operator+(U k) const {
    check_runtime();
    return operations::add[m_type_index](ptr, operand{k});
  }

  template <numeric_operand U>
  double operator-(U k) const {
    check_runtime();
    return operations::subtract[m_type_index](ptr, operand{k});
  }

  template <numeric_operand U>
  double operator*(U k) const {
    check_runtime();
    return operations::multiply[m_type_index](ptr, operand{k});
  }

  template <numeric_operand U>
  double operator/(U k) const {
    check_runtime();
    return operations::divide[m_type_index](ptr, operand{k});
  }

  template <numeric_operand U>
  std::partial_ordering operator<=>(U k) const {
    check_runtime();
    return operations::compare[m_type_index](ptr, operand{k});
  }

  template <numeric_operand U>
  bool operator==(U k) const {
    return (*this <=> k) == 0;
  }

  template <numeric_operand U>
  aligned_iterator_storage& operator+=(U k) {
    check_runtime();
    operations::add_assign[m_type_index](ptr, operand{k});
    return *this;
  }

  template <numeric_operand U>
  aligned_iterator_storage& operator-=(U k) {
    check_runtime();
    operations::subtract_assign[m_type_index](ptr, operand{k});
    return *this;
  }

 private:
  void check_runtime() const {
    if (m_type_index >= sizeof...(Ty)) {
      throw exception(error::index_exceed);
    }
    if (ptr == nullptr) {
      throw exception(error::null_pointer);
    }
  }

  template <class T>
  struct op_set {
    template <char Op>
    static double arithmetic(std::byte* slot, operand k) {
      if constexpr (std::is_arithmetic_v<T>) {
        if constexpr (Op == '/') {
          if (k.is_float ? k.d == 0.0 : k.u == 0) {
            throw exception(error::division_by_zero);
          }
        }
        const double value = static_cast<double>(
            *std::launder(reinterpret_cast<T*>(slot)));
        const double rhs = k.as_double();
        if constexpr (Op == '+') return value + rhs;
        if constexpr (Op == '-') return value - rhs;
        if constexpr (Op == '*') return value * rhs;
        if constexpr (Op == '/') return value / rhs;
      } else {
        throw exception(error::unsupported_operation);
      }
    }

    static std::partial_ordering compare(std::byte* slot, operand k) {
      if constexpr (std::is_arithmetic_v<T>) {
        const auto value = *std::launder(reinterpret_cast<T*>(slot));
        if (k.is_float) {
// Floating operand: long double represents uint64 and double exactly, so integer slots need
// no narrowing to double first; NaN falls out of the float comparison as unordered.
          return static_cast<long double>(value) <=> k.as_long_double();
        }
        if constexpr (std::is_floating_point_v<T>) {
          if (std::isnan(value)) return std::partial_ordering::unordered;
          if (value < 0) return std::partial_ordering::less;
          // Check before converting: UINT64_MAX rounds up to 2^64 in double.
          if (value >= static_cast<T>(0x1p64L)) {
            return std::partial_ordering::greater;
          }
          const auto integer = static_cast<std::uint64_t>(value);
          if (integer < k.u) return std::partial_ordering::less;
          if (integer > k.u) return std::partial_ordering::greater;
          return value == static_cast<T>(integer)
              ? std::partial_ordering::equivalent
              : std::partial_ordering::greater;
        } else {
          if constexpr (std::is_signed_v<T>) {
            if (value < 0) return std::partial_ordering::less;
          }
          return static_cast<std::uint64_t>(value) <=> k.u;
        }
      } else {
        throw exception(error::unsupported_operation);
      }
    }

    template <bool Subtract>
    static void update(std::byte* slot, operand k) {
      if constexpr (std::is_arithmetic_v<T> && !std::is_const_v<T>) {
        auto& target = *std::launder(reinterpret_cast<T*>(slot));
        using value_type = std::remove_cv_t<T>;
        const value_type value = target;
        if constexpr (std::is_integral_v<T>) {
// Normalize the operand into sign/magnitude first, so integers and floats share one range check.
// A floating operand must be integral: non-integral increments are rejected, never truncated.
          bool subtract = Subtract;
          std::uint64_t delta = 0;
          if (k.is_float) {
            long double exact = static_cast<long double>(k.d);
            if (Subtract) exact = -exact;
            if (std::trunc(exact) != exact) {
              throw exception(error::unsupported_operation);
            }
            if (exact < 0) {
              subtract = !subtract;
              exact = -exact;
            }
            if (exact >= static_cast<long double>(0x1p64L)) {
              throw exception(error::arithmetic_overflow);
            }
            delta = static_cast<std::uint64_t>(exact);
          } else {
            delta = k.u;
          }
          bool negative = false;
          auto magnitude = static_cast<std::uint64_t>(value);
          if constexpr (std::is_signed_v<T>) {
            if (value < 0) {
              negative = true;
              magnitude = std::uint64_t{0} - magnitude;
            }
          }
          // Sign/magnitude arithmetic avoids signed overflow and rhs narrowing.
          if (negative == subtract) {
            if (delta > std::numeric_limits<std::uint64_t>::max() - magnitude) {
              throw exception(error::arithmetic_overflow);
            }
            magnitude += delta;
          } else if (delta > magnitude) {
            magnitude = delta - magnitude;
            negative = !negative;
          } else {
            magnitude -= delta;
          }
          if (negative && magnitude != 0) {
            if constexpr (std::is_signed_v<T>) {
              constexpr auto limit = std::uint64_t{0} -
                  static_cast<std::uint64_t>(std::numeric_limits<T>::min());
              if (magnitude > limit) {
                throw exception(error::arithmetic_overflow);
              }
              target = static_cast<value_type>(
                  -1 - static_cast<value_type>(magnitude - 1));
            } else {
              throw exception(error::arithmetic_overflow);
            }
          } else {
            if (magnitude > static_cast<std::uint64_t>(
                                std::numeric_limits<T>::max())) {
              throw exception(error::arithmetic_overflow);
            }
            target = static_cast<value_type>(magnitude);
          }
        } else {
          const value_type rhs = static_cast<value_type>(k.as_double());
          const value_type result = Subtract ? value - rhs : value + rhs;
          if (std::isfinite(value) && !std::isfinite(result)) {
            throw exception(error::arithmetic_overflow);
          }
          target = result;
        }
      } else {
        throw exception(error::unsupported_operation);
      }
    }
  };

  template <class... Ops>
  struct operation_tables {
    using arithmetic_fn = double (*)(std::byte*, operand);
    using compare_fn = std::partial_ordering (*)(std::byte*, operand);
    using update_fn = void (*)(std::byte*, operand);

    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> add = {
        &Ops::template arithmetic<'+'>...};
    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> subtract = {
        &Ops::template arithmetic<'-'>...};
    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> multiply = {
        &Ops::template arithmetic<'*'>...};
    inline static constexpr std::array<arithmetic_fn, sizeof...(Ops)> divide = {
        &Ops::template arithmetic<'/'>...};
    inline static constexpr std::array<compare_fn, sizeof...(Ops)> compare = {
        &Ops::compare...};
    inline static constexpr std::array<update_fn, sizeof...(Ops)> add_assign = {
        &Ops::template update<false>...};
    inline static constexpr std::array<update_fn, sizeof...(Ops)> subtract_assign = {
        &Ops::template update<true>...};
  };

  using op_sets =
      typename exp_list<stored_type_t<Ty>...>::template for_each<op_set>;
  using operations = typename op_sets::template to<operation_tables>;

// All slot tests go through here: Ty... is normalized first (Ty& -> std::reference_wrapper<Ty>).
  template <class T>
  static constexpr bool slot_type_allowed() {
    return is_slot_convertible<T, stored_type_t<Ty>...>;
  }

// Whether the current slot is usable as T (bitmap only; the caller guarantees the index is in range).
// With <= 64 types this stays one 64-bit shift (identical to the 64-bit version);
// wider lists take the bit from one half with index & 63, which also performs the minus 64.
  template <class T>
  bool slot_mask_hit() const {
    constexpr auto mask = type_mask_series<T, stored_type_t<Ty>...>;
    if constexpr (sizeof...(Ty) <= 64) {
      return ((mask.low >> m_type_index) & 1) != 0;
    } else {
      const std::uint64_t half = m_type_index < 64 ? mask.low : mask.high;
      return ((half >> (m_type_index & 63)) & 1) != 0;
    }
  }
};

// ============================================================================
// 4) Forward and reverse iterators
// ============================================================================

// Old spelling: meta_pipe<...>::all_to<meta_aligned_iterator<>>::run<meta_ostream<>>::type::to_t
// The new spelling is identical - the pipe is implemented in exactly this shape.
// Ty... is normalized first: int& means std::reference_wrapper<int>.
template <std::size_t Alignment = 0, class... Ty>
using aligned_offset_stream =
    typename meta_pipe<meta_istream_list<stored_type_t<Ty>...>>
        ::template all_to<meta_aligned_iterator<Alignment>>
        ::template run<meta_ostream<>>::type::to_t;

// Reverse offset stream: meta_rostream prepends each step seek_to, so the resulting list already
// is a reversed table of absolute coordinates - entry j is the seek_to of reverse slot j, with
// value as the offset from the buffer start, type as that slot type and destroy built in.
template <std::size_t Alignment = 0, class... Ty>
using reverse_aligned_offset_stream =
    typename meta_pipe<meta_istream_list<stored_type_t<Ty>...>>
        ::template all_to<meta_aligned_iterator<Alignment>>
        ::template run<meta_rostream<>>::type::to::type;

// Whether the layout can be accessed safely: Alignment keeps every cell aligned to alignof(T), or
// the target accepts unaligned access. On weakly aligned CPUs (old ARM/MIPS/SPARC) a packed
// layout is a compile-time error instead of a run-time fault. The test matches
// meta_iostream::layout_is_aligned: alignof(T) | Alignment.
// Note: type_safe_aligned_range uses reference semantics (get/emplace dereference), so it needs
// this check; for wire data use meta_wire_iterator (by value plus byte copies, no dereference).
template <std::size_t Alignment, class... Ty>
inline constexpr bool layout_access_ok =
    meta_iostream::unaligned_access_ok ||
    (meta_iostream::layout_is_aligned<stored_type_t<Ty>, Alignment> && ...);

namespace layout_detail {

// Static facts of the aligned layout: both iterators share this one, and addresses always use absolute offsets.
template <std::size_t Alignment, class... Ty>
struct aligned_layout {
// offsets[i] is the absolute byte offset of slot i from the buffer start (not a neighbour delta).
  static constexpr auto offsets =
      to_meta_array_t<aligned_offset_stream<Alignment, Ty...>>::array();
  static constexpr std::size_t size = sizeof...(Ty);
// Total layout bytes = the position after the last slot, which is also where end / rend point.
  static constexpr std::size_t total_size = field_size<Alignment, Ty...>();

  static constexpr std::size_t offset_at(std::size_t index) {
    return index == size ? total_size : offsets[index];
  }
};

// Slot coordinate table: seek_to already carries the absolute offset and the type, so destruction
// lets it do the work instead of re-running the type stream to find which type a cell holds.
template <class SeekList, class IndexSeq>
struct slot_table;

template <class... Seek, std::size_t... I>
struct slot_table<exp_list<Seek...>, std::index_sequence<I...>> {
  static void destroy_at(std::size_t index, std::byte* base_ptr) {
// The matched cell is destroyed; the fold boolean result is unused and discarded explicitly.
// [[maybe_unused]] cannot express this (it does not apply to statements), and Clang
// -Wunused-value wants the result cast to void.
    (void)((index == I ? (Seek{}.destroy(base_ptr), true) : false) || ...);
  }
};

// Every entry in the coordinate table remembers its cell type (seek_to<absolute offset, T>::type).
template <class Seek>
using seek_type = typename Seek::type;

}  // namespace layout_detail

template <std::size_t Alignment = 0, class... Ty>
class type_safe_aligned_iterator {
  static_assert(sizeof...(Ty) > 0,
                "type_safe_aligned_iterator requires at least one type");
  static_assert(layout_access_ok<Alignment, Ty...>,
                "type_safe_aligned_iterator: packed layout on a target without "
                "unaligned access (use meta_wire_iterator for wire data, or "
                "alignment >= alignof(T))");

  using layout = layout_detail::aligned_layout<Alignment, Ty...>;

  std::byte* base_ptr_{nullptr};
// storage_.m_type_index is the only index; storage_.ptr is always base + offsets[slot],
// i.e. a cached absolute offset rather than an accumulated neighbour delta.
  aligned_iterator_storage<Ty...> storage_{};

  void refresh() {
    storage_.ptr = base_ptr_ == nullptr
                       ? nullptr
                       : base_ptr_ + layout::offset_at(storage_.m_type_index);
  }

// Advance one slot: relocate through the absolute offset table, never by adding a neighbour delta.
  type_safe_aligned_iterator& advance() {
    using storage_type = aligned_iterator_storage<Ty...>;
    if (storage_.m_type_index >= layout::size) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (base_ptr_ == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    ++storage_.m_type_index;
    refresh();
    return *this;
  }

 public:
// The constructor is explicit: both the buffer start and the slot index must be given.
// type_index == sizeof...(Ty) marks end(); a null pointer is only diagnosed when a slot is accessed.
  explicit type_safe_aligned_iterator(std::byte* base_ptr,
                                      std::size_t type_index) : base_ptr_(base_ptr) {
    if (type_index > layout::size) {
      using storage_type = aligned_iterator_storage<Ty...>;
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    storage_.m_type_index = type_index;
    refresh();
  }

// Dereferencing yields the checked proxy of the current slot (ptr already positioned by absolute offset).
  aligned_iterator_storage<Ty...>& operator*() { return storage_; }

// Prefix ++: move to the next slot of the aligned layout.
  type_safe_aligned_iterator& operator++() { return advance(); }

  bool operator==(type_safe_aligned_iterator const& other) const {
    return base_ptr_ == other.base_ptr_ &&
           storage_.m_type_index == other.storage_.m_type_index;
  }

  bool operator!=(type_safe_aligned_iterator const& other) const {
    return !(*this == other);
  }
};

// Reverse iterator: consumes the reversed absolute coordinate table produced by meta_rostream.
//   - the state is the reverse index: ++ increments it, rend is sizeof...(Ty), same as std::reverse_iterator;
//   - addresses come from that table offsets[j] (absolute), never through the forward table;
//   - the checked proxy is instantiated over the reversed type list, so reverse index j is its bitmap bit;
//     no conversion to a forward index is needed;
//   - destroy lets that slot seek_to do the work.
template <std::size_t Alignment = 0, class... Ty>
class type_safe_aligned_reverse_iterator {
  static_assert(sizeof...(Ty) > 0,
                "type_safe_aligned_reverse_iterator requires at least one type");
  static_assert(layout_access_ok<Alignment, Ty...>,
                "type_safe_aligned_reverse_iterator: packed layout on a target "
                "without unaligned access (use meta_wire_iterator for wire data, "
                "or alignment >= alignof(T))");

  using seek_list = reverse_aligned_offset_stream<Alignment, Ty...>;
  static constexpr auto offsets = to_meta_array_t<seek_list>::array();
// Reversed type list: reverse slot j maps to bit j of aligned_iterator_storage<Ty_reversed...>.
  using reversed_types =
      typename seek_list::template for_each<layout_detail::seek_type>;
  using proxy_type = typename reversed_types::template to<aligned_iterator_storage>;
  using slot_table =
      layout_detail::slot_table<seek_list, std::index_sequence_for<Ty...>>;

  std::byte* base_ptr_{nullptr};
// Reverse index: 0 = the last cell of the layout, sizeof...(Ty) = rend.
  std::size_t reverse_index_{0};
// The checked proxy materialized by operator* (reference semantics, hence a member).
  proxy_type storage_{};

 public:
  explicit type_safe_aligned_reverse_iterator(std::byte* base_ptr,
                                              std::size_t reverse_index)
      : base_ptr_(base_ptr), reverse_index_(reverse_index) {
    if (reverse_index > sizeof...(Ty)) {
      using storage_type = proxy_type;
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
  }

// Dereference: entry j already is the absolute offset of reverse slot j, and the slot index is j.
  proxy_type& operator*() {
    if (reverse_index_ == sizeof...(Ty)) {
      storage_.ptr = base_ptr_;
    } else {
      storage_.ptr = base_ptr_ == nullptr
                         ? nullptr
                         : base_ptr_ + offsets[reverse_index_];
    }
    storage_.m_type_index = reverse_index_;
    return storage_;
  }

// Destroys the object on the current slot: the coordinate object carries offset and type.
  void destroy() {
    using storage_type = proxy_type;
    if (reverse_index_ >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (base_ptr_ == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    slot_table::destroy_at(reverse_index_, base_ptr_);
  }

  type_safe_aligned_reverse_iterator& operator++() {
    using storage_type = proxy_type;
    if (reverse_index_ >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
    if (base_ptr_ == nullptr) {
      throw typename storage_type::exception(storage_type::error::null_pointer);
    }
    ++reverse_index_;
    return *this;
  }

  bool operator==(type_safe_aligned_reverse_iterator const& other) const {
    return base_ptr_ == other.base_ptr_ && reverse_index_ == other.reverse_index_;
  }

  bool operator!=(type_safe_aligned_reverse_iterator const& other) const {
    return !(*this == other);
  }
};

// ============================================================================
// 5) Range adapters
// ============================================================================
// Alignment is the same parameter as for the iterators and field_size: 0 aligns every cell by
// alignof (memory layout), a power of two uses that boundary (1 = packed). initialize / begin /
// rbegin all follow it.
template <std::size_t Alignment = 0, class... Ty>
class type_safe_aligned_range {
  static_assert(layout_access_ok<Alignment, Ty...>,
                "type_safe_aligned_range: packed layout on a target without "
                "unaligned access (use meta_wire_iterator for wire data, or "
                "alignment >= alignof(T))");

 public:
  static constexpr std::size_t bytes = field_size<Alignment, Ty...>();
  type_safe_aligned_range() = delete;  // no default construction; a base pointer is required
  // base_ptr must address storage large enough for the whole Ty... layout.
  // References may be written directly in Ty... (int&, say): layout and storage use
  // std::reference_wrapper, but values still require std::ref / std::cref.
  explicit type_safe_aligned_range(std::byte* base_ptr) : base_ptr_(base_ptr) {}

  type_safe_aligned_iterator<Alignment, Ty...> begin() const {
    return type_safe_aligned_iterator<Alignment, Ty...>(base_ptr_, 0);
  }

  type_safe_aligned_iterator<Alignment, Ty...> end() const {
    return type_safe_aligned_iterator<Alignment, Ty...>(base_ptr_,
                                                       sizeof...(Ty));
  }

  type_safe_aligned_reverse_iterator<Alignment, Ty...> rbegin() const {
    return type_safe_aligned_reverse_iterator<Alignment, Ty...>(base_ptr_, 0);
  }

  type_safe_aligned_reverse_iterator<Alignment, Ty...> rend() const {
    return type_safe_aligned_reverse_iterator<Alignment, Ty...>(
        base_ptr_, sizeof...(Ty));
  }

  std::byte* data() const { return base_ptr_; }

// Number of slots (elements in the layout).
  static constexpr std::size_t size() noexcept { return sizeof...(Ty); }

// ---- Run-time index: r[i] ----
// Returns the checked proxy by value (two machine words: ptr plus slot index).
  //
// Why not Ty_i&: the cells are heterogeneous, so a range element has no single type. Returning a
// reference would require a mutable proxy member inside the range (mutable state in a const
// object, two iterators sharing one proxy, races under concurrency), so it returns by value.
  //
// This does not conflict with range semantics: std::array has both operator[] and std::get<I>;
// here "uniform run-time access" and "exact compile-time access" are two entry points
// (run time cannot yield Ty_i& - see get<I> below).
  //
// Dereferencing keeps the same checks: out-of-range throws at operator[], null or type mismatch at the proxy.
  aligned_iterator_storage<Ty...> operator[](std::size_t index) const {
    using storage_type = aligned_iterator_storage<Ty...>;
    if (index >= sizeof...(Ty)) {
      throw typename storage_type::exception(storage_type::error::index_exceed);
    }
// Iterator construction does the same absolute-offset positioning that ++ uses, over the same table.
    type_safe_aligned_iterator<Alignment, Ty...> it(base_ptr_, index);
  return *it;  // copied out: it is a temporary, so no reference may escape
  }

// Type known, index still run time: validate the slot first, then access through a constant
// absolute offset. Exception priority matches the proxy: out of range, null, type mismatch.
  template <class T>
  T& at(std::size_t index) const {
    using storage_type = aligned_iterator_storage<Ty...>;
    using exception = typename storage_type::exception;
    if constexpr (!is_slot_convertible<T, stored_type_t<Ty>...>) {
      static_assert(is_slot_convertible<T, stored_type_t<Ty>...>,
                    "T must be one of aligned_iterator_storage's types "
                    "(or a const-qualified version of one)");
    } else {
      constexpr auto mask = type_mask_series<T, stored_type_t<Ty>...>;
      constexpr bool unique =
          (mask.high == 0 && std::has_single_bit(mask.low)) ||
          (mask.low == 0 && std::has_single_bit(mask.high));
      if constexpr (unique) {
        constexpr std::size_t slot = mask.low != 0
            ? std::countr_zero(mask.low)
            : 64 + std::countr_zero(mask.high);
// Matching a unique slot also proves the index is in range; the failure branch keeps the original priority.
        if (index != slot || base_ptr_ == nullptr) {
          throw exception(index >= sizeof...(Ty) ? storage_type::error::index_exceed
              : base_ptr_ == nullptr ? storage_type::error::null_pointer
                                    : storage_type::error::type_not_safe);
        }
        using cursor =
            typename aligned_offset_stream<Alignment, Ty...>::template at<slot>;
        return *std::launder(reinterpret_cast<std::remove_reference_t<T>*>(
            cursor{}.address_of(base_ptr_)));
      } else {
        return static_cast<T&>((*this)[index]);
      }
    }
  }

// ---- Compile-time index: r.get<I>() / get<I>(r) / structured bindings ----
// Returns the exact type reference of that slot (the normalized Ty... type).
// The cursor is the same seek_to used by slot_at<I>: base + constant offset + launder, with
// no run-time checks and no extra table lookup for any Alignment.
  //
// Note const: the range is a view and constness does not propagate to elements (as with
// std::span, whose data() const returns a writable pointer), so get<I>() on a const range is still T&.
  template <std::size_t I>
  auto& get() const noexcept {
    static_assert(I < sizeof...(Ty), "type_safe_aligned_range::get<I>: slot index out of range");
    using cursor = typename aligned_offset_stream<Alignment, Ty...>::template at<I>;
    return cursor{}.get(base_ptr_);
  }

 private:
  std::byte* base_ptr_;
};

// The cursor of cell I (that cell seek_to): the offset is a compile-time constant, so run time
// is just base + value. Never walk I steps with a run-time iterator for a constant index.
template <std::size_t I, class Range>
struct slot_cursor;

template <std::size_t I, std::size_t Alignment, class... Ty>
struct slot_cursor<I, type_safe_aligned_range<Alignment, Ty...>> {
  static_assert(I < sizeof...(Ty), "slot_cursor: slot index out of range");
  using type = typename aligned_offset_stream<Alignment, Ty...>::template at<I>;
};

template <std::size_t I, class Range>
using slot_cursor_t = typename slot_cursor<I, Range>::type;

// Convenience: slot_at<I>(range).get(range.data()) / .address_of(range.data());
// the range only participates in type deduction and costs nothing at run time.
template <std::size_t I, class Range>
constexpr slot_cursor_t<I, Range> slot_at(Range const&) noexcept {
  return {};
}

// get<I>(range): the ADL entry for compile-time indexing, equivalent to range.get<I>().
// Structured bindings and generic code use it (together with tuple_size / tuple_element below).
template <std::size_t I, std::size_t Alignment, class... Ty>
auto& get(type_safe_aligned_range<Alignment, Ty...> const& range) noexcept {
  static_assert(I < sizeof...(Ty), "get<I>(range): slot index out of range");
  return range.template get<I>();
}


// ============================================================================
// 6) Construct every element (on exception, clean up the constructed prefix in reverse)
// ============================================================================
template <std::size_t Alignment, class... Ty, class... Args>
void initialize(type_safe_aligned_range<Alignment, Ty...> const& range,
                Args&&... args) {
  static_assert(sizeof...(Ty) == sizeof...(Args),
                "initialize requires one argument for each range element");
  auto* data = range.data();
// Reference slots are normalized to std::reference_wrapper:
// std::ref(x) / std::cref(x) must be passed here; passing x itself fails to construct.
// node_con<unchanged, true> keeps stream_cond64's eol stop but undoes its window-2
// init-failure rule, because meta_aligned_iterator initializes by receiving the
// first type (it is not pre-initialized like the looper's receivers).
  meta_states::run<meta_ios::node_con<meta_stream64::unchanged, true>,
                   meta_aligned_iterator<Alignment>,
                   meta_istream_list<stored_type_t<Ty>...>>::for_each_forward(
      [&range, data]<class Stream, class Arg>(Stream ptr_stream, Arg&& value) {
        try {
          ptr_stream.object().emplace(data, std::forward<Arg>(value));
        } catch (...) {
// ptr_stream.index() is this round (0-based) slot index; the first index elements are already
// constructed, so destroy them in reverse and skip the current and later (unconstructed) slots.
          constexpr auto index = ptr_stream.index();
          auto destroy_iter = range.rbegin();
          for (std::size_t i = 0; i < sizeof...(Ty) - index; ++i) {
            ++destroy_iter;
          }
          for (; destroy_iter != range.rend(); ++destroy_iter) {
            destroy_iter.destroy();
          }
          throw;
        }
      },
      std::forward<Args>(args)...);
}
// ============================================================================
// 6.b) Destroy every element (on exception, clean up the constructed prefix in reverse)
// ============================================================================
template <std::size_t Alignment, class... Ty>
void destroy_range(type_safe_aligned_range<Alignment, Ty...>const& range){
  using source = meta_iostream::meta_istream<reverse_aligned_offset_stream<Alignment, Ty...>>;
  meta_states::run<
    meta_ios::stream_states_co<>,
    meta_iostream::meta_iterator,
    source>
    ::for_each([&](auto ptr_stream){
      ptr_stream.object().destroy(range.data());
    });
}

// ============================================================================
// 7) Read and write by the current slot type
// ============================================================================
template <class... Ty>
std::ostream& operator<<(std::ostream& os,
                         aligned_iterator_storage<Ty...> const& storage) {
  using storage_type = aligned_iterator_storage<Ty...>;
  if (storage.m_type_index >= sizeof...(Ty)) {
    throw typename storage_type::exception(storage_type::error::index_exceed);
  }
  meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                   meta_istream_list<stored_type_t<Ty>...>>::for_each(
      [&os, &storage]<class Stream>(Stream s) {
        using current_type = typename Stream::to_t;
        if (os && s.index() == storage.m_type_index &&
            storage.template safe<current_type>()) {
          os << static_cast<current_type&>(storage);
        }
      });
  return os;
}

template <class... Ty>
std::istream& operator>>(std::istream& is,
                         aligned_iterator_storage<Ty...>& storage) {
  using storage_type = aligned_iterator_storage<Ty...>;
  if (storage.m_type_index >= sizeof...(Ty)) {
    throw typename storage_type::exception(storage_type::error::index_exceed);
  }
  meta_states::run<meta_ios::stream_states_co<>, meta_iterator,
                   meta_istream_list<stored_type_t<Ty>...>>::for_each(
      [&is, &storage]<class Stream>(Stream s) {
        using current_type = typename Stream::to_t;
        if (is && s.index() == storage.m_type_index &&
            storage.template safe<current_type>()) {
          is >> static_cast<current_type&>(storage);
        }
      });
  return is;
}

}  // namespace type_safe_aligned

// ============================================================================
// 7) tuple-like protocol: structured bindings / std::tuple_size / std::tuple_element
// ============================================================================
// type_safe_aligned_range is two things at once:
//   - a range: begin/end/rbegin/rend plus the run-time r[i] (uniform checked proxy)
//   - tuple-like: the compile-time get<I>(r) (exact type reference, no checks)
// std::array has exactly this shape - operator[] and get<I> coexist because "uniform run-time
// access" and "compile-time heterogeneous access" are different entry points.
//
// With the two specializations below, structured bindings bind straight to the real objects in the buffer:
//   auto& [magic, version, kind, flags, device, ts, value, length] = range;
//
// View semantics: elements of a const range stay writable (data() const returns a writable pointer,
// as with std::span), so the const version maps to the same element type, not const T.
// That also keeps auto const& [..] = range; bindings writable, matching get<I>().
namespace std {

template <std::size_t Alignment, class... Ty>
struct tuple_size<type_safe_aligned::type_safe_aligned_range<Alignment, Ty...>>
    : integral_constant<std::size_t, sizeof...(Ty)> {};

template <std::size_t Alignment, class... Ty>
struct tuple_size<
    type_safe_aligned::type_safe_aligned_range<Alignment, Ty...> const>
    : tuple_size<type_safe_aligned::type_safe_aligned_range<Alignment, Ty...>> {
};

template <std::size_t I, std::size_t Alignment, class... Ty>
struct tuple_element<
    I, type_safe_aligned::type_safe_aligned_range<Alignment, Ty...>> {
  static_assert(I < sizeof...(Ty), "tuple_element: slot index out of range");
// at<I> is that cell seek_to; its type is the normalized stored type.
  using type = typename type_safe_aligned::aligned_offset_stream<
      Alignment, Ty...>::template at<I>::type;
};

template <std::size_t I, std::size_t Alignment, class... Ty>
struct tuple_element<
    I, type_safe_aligned::type_safe_aligned_range<Alignment, Ty...> const>
    : tuple_element<I,
                    type_safe_aligned::type_safe_aligned_range<Alignment,
                                                              Ty...>> {};

}  // namespace std
