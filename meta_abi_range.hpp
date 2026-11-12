#pragma once

#include "type_safe_aligned_iterator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace meta_abi {

struct x86_control {
  std::uint32_t mxcsr;
  std::uint16_t x87_control_word;
  std::uint16_t reserved;
};

struct alignas(16) win_x64_fp_state {
  std::array<std::array<std::byte, 16>, 10> xmm6_xmm15;
  x86_control control;
};

struct alignas(16) x86_fp_state {
  x86_control control;
};

struct win_x64_gpr_state {
  std::uint64_t rbx, rbp, rdi, rsi, r12, r13, r14, r15;
};

struct sysv_x64_gpr_state {
  std::uint64_t rbx, rbp, r12, r13, r14, r15;
};

struct x86_gpr_state {
  std::uint32_t ebx, ebp, esi, edi;
};

struct alignas(16) aapcs64_fp_state {
  std::array<std::uint64_t, 8> d8_d15;
  std::uint32_t fpcr, fpsr;
};

struct aapcs64_gpr_state {
  // x18 is platform-defined; only restore it where the platform permits it.
  std::array<std::uint64_t, 13> x18_x30;
};

struct alignas(16) aapcs32_fp_state {
  std::array<std::uint64_t, 8> d8_d15;
  std::uint32_t fpscr;
};

struct aapcs32_gpr_state {
  std::array<std::uint32_t, 8> r4_r11;
  std::uint32_t lr;
};

struct alignas(16) riscv_fp_state {
  std::array<std::uint64_t, 12> fs0_fs11;
  std::uint32_t fcsr;
};

template <class Word>
struct riscv_gpr_state {
  std::array<Word, 12> s0_s11;
  Word ra;
};

struct alignas(16) x64_bootstrap {
  std::uint64_t resume_pc, exit_pc;
};

struct alignas(16) win_x64_bootstrap {
  std::uint64_t resume_pc, exit_pc;
  std::array<std::byte, 32> shadow_space;
};

struct alignas(16) x86_bootstrap {
  std::array<std::byte, 8> reserved;
  std::uint32_t resume_pc, exit_pc;
};

namespace detail {

template <class Word, class Bootstrap, class Fp, class Gpr,
          std::size_t ShadowBytes, std::size_t RedZoneBytes>
struct x86_abi {
  using word_type = Word;
  using bootstrap_type = Bootstrap;
  using fp_type = Fp;
  using gpr_type = Gpr;
  using fields = exp_utilities::exp_list<Bootstrap, Fp, Gpr, Word>;
  static constexpr bool stack_return = true;
  static constexpr bool has_simd = ShadowBytes != 0;
  static constexpr std::size_t sp_index = 3;
  static constexpr std::size_t fp_index = 1;
  static constexpr std::size_t gpr_index = 2;
  static constexpr std::size_t shadow_bytes = ShadowBytes;
  static constexpr std::size_t red_zone_bytes = RedZoneBytes;
};

template <class Word, class Fp, class Gpr, std::size_t LinkOffset>
struct link_abi {
  using word_type = Word;
  using fp_type = Fp;
  using gpr_type = Gpr;
  using fields = exp_utilities::exp_list<Gpr, Fp, Word, Word>;
  static constexpr bool stack_return = false;
  static constexpr bool has_simd = true;
  static constexpr std::size_t sp_index = 2;
  static constexpr std::size_t fp_index = 1;
  static constexpr std::size_t gpr_index = 0;
  static constexpr std::size_t pc_index = 3;
  static constexpr std::size_t link_offset = LinkOffset;
  static constexpr std::size_t shadow_bytes = 0;
  static constexpr std::size_t red_zone_bytes = 0;
};

// Bind the existing aligned-range primitives to an ABI's field list.
template <std::size_t N, class Fields>
struct range_layout;

template <std::size_t N, class... Fields>
struct range_layout<N, exp_utilities::exp_list<Fields...>> {
  using frame_range_type =
      type_safe_aligned::type_safe_aligned_range<0, Fields...>;
  static constexpr std::size_t frame_bytes =
      type_safe_aligned::field_size<0, Fields...>();
  static constexpr std::size_t aligned_bytes = N & ~std::size_t{15};
  static constexpr std::size_t stack_bytes =
      aligned_bytes >= frame_bytes ? aligned_bytes - frame_bytes : 0;
  using range_type = type_safe_aligned::type_safe_aligned_range<
      0, std::array<std::byte, stack_bytes>, Fields...>;

  template <std::size_t I>
  static constexpr std::size_t frame_offset =
      type_safe_aligned::slot_cursor_t<I, frame_range_type>::value;
};

}  // namespace detail

using win_x64 = detail::x86_abi<std::uint64_t, win_x64_bootstrap,
                               win_x64_fp_state, win_x64_gpr_state, 32, 0>;
using sysv_x64 = detail::x86_abi<std::uint64_t, x64_bootstrap,
                                x86_fp_state, sysv_x64_gpr_state, 0, 128>;
using win_x86 = detail::x86_abi<std::uint32_t, x86_bootstrap,
                               x86_fp_state, x86_gpr_state, 0, 0>;
using sysv_x86 = detail::x86_abi<std::uint32_t, x86_bootstrap,
                                x86_fp_state, x86_gpr_state, 0, 0>;
using aapcs64 = detail::link_abi<std::uint64_t, aapcs64_fp_state,
                                aapcs64_gpr_state, 12 * sizeof(std::uint64_t)>;
using aapcs32_vfp = detail::link_abi<std::uint32_t, aapcs32_fp_state,
                                    aapcs32_gpr_state,
                                    offsetof(aapcs32_gpr_state, lr)>;
using riscv64_lp64d = detail::link_abi<std::uint64_t, riscv_fp_state,
                                      riscv_gpr_state<std::uint64_t>,
                                      12 * sizeof(std::uint64_t)>;
using riscv32_ilp32d = detail::link_abi<std::uint32_t, riscv_fp_state,
                                       riscv_gpr_state<std::uint32_t>,
                                       12 * sizeof(std::uint32_t)>;

template <std::size_t N, class Abi>
class meta_abi_range {
  using layout = detail::range_layout<N, typename Abi::fields>;
  static_assert(layout::stack_bytes >= 16 + Abi::red_zone_bytes,
                "meta_abi_range: N is too small for the frame and working stack");

 public:
  using abi_type = Abi;
  using word_type = typename Abi::word_type;
  using fp_type = typename Abi::fp_type;
  using gpr_type = typename Abi::gpr_type;
  using range_type = typename layout::range_type;
  using frame_range_type = typename layout::frame_range_type;

  static constexpr std::size_t alignment = 16;
  static constexpr std::size_t storage_bytes = N;
  static constexpr std::size_t layout_bytes = layout::aligned_bytes;
  static constexpr std::size_t frame_bytes = layout::frame_bytes;
  static constexpr std::size_t frame_offset = layout::stack_bytes;
  static constexpr std::size_t shadow_bytes = Abi::shadow_bytes;
  static constexpr std::size_t red_zone_bytes = Abi::red_zone_bytes;
  static constexpr std::size_t sp_offset =
      frame_offset + layout::template frame_offset<Abi::sp_index>;
  static constexpr std::size_t gpr_offset =
      frame_offset + layout::template frame_offset<Abi::gpr_index>;
  static constexpr std::size_t fp_offset =
      frame_offset + layout::template frame_offset<Abi::fp_index>;
  static constexpr std::size_t ret_address_offset = [] {
    if constexpr (Abi::stack_return) {
      return frame_offset + offsetof(typename Abi::bootstrap_type, resume_pc);
    } else {
      return frame_offset + layout::template frame_offset<Abi::pc_index>;
    }
  }();
  static constexpr std::size_t return_address_offset = [] {
    if constexpr (Abi::stack_return) {
      return frame_offset + offsetof(typename Abi::bootstrap_type, exit_pc);
    } else {
      return gpr_offset + Abi::link_offset;
    }
  }();
  static constexpr std::size_t initial_sp_offset = [] {
    if constexpr (Abi::stack_return) {
      return ret_address_offset;
    } else {
      return frame_offset;
    }
  }();
  static constexpr std::size_t stack_top_offset = [] {
    if constexpr (Abi::stack_return) {
      return return_address_offset;
    } else {
      return frame_offset;
    }
  }();
  static constexpr std::size_t usable_stack_bytes = stack_top_offset;

  // Like type_safe_aligned_range, the pointer overload requires enough storage.
  explicit meta_abi_range(std::byte* data) : range_(checked_data(data)) {}

  explicit meta_abi_range(std::span<std::byte> storage)
      : meta_abi_range(checked_storage(storage)) {}

  void initialize() const noexcept { std::memset(data(), 0, N); }

  std::byte* data() const noexcept { return range_.data(); }
  std::byte* stack_bottom() const noexcept { return layout_end(); }
  std::byte* stack_limit() const noexcept { return data(); }
  std::byte* stack_top() const noexcept { return data() + stack_top_offset; }
  std::byte* initial_sp() const noexcept { return data() + initial_sp_offset; }
  std::byte* sp() const noexcept { return data() + sp_offset; }
  std::byte* ret_address() const noexcept { return data() + ret_address_offset; }
  std::byte* return_address() const noexcept {
    return data() + return_address_offset;
  }
  std::byte* frame() const noexcept { return data() + frame_offset; }
  std::byte* gpr_regs() const noexcept { return data() + gpr_offset; }
  std::byte* fp_regs() const noexcept { return data() + fp_offset; }
  std::byte* storage_end() const noexcept { return data() + N; }
  std::byte* layout_end() const noexcept { return data() + layout_bytes; }

  std::byte* fp_control() const noexcept {
    if constexpr (Abi::stack_return) {
      return fp_regs() + offsetof(fp_type, control);
    } else if constexpr (std::is_same_v<Abi, aapcs64>) {
      return fp_regs() + offsetof(fp_type, fpcr);
    } else if constexpr (std::is_same_v<Abi, aapcs32_vfp>) {
      return fp_regs() + offsetof(fp_type, fpscr);
    } else {
      return fp_regs() + offsetof(fp_type, fcsr);
    }
  }

  std::byte* simd_regs() const noexcept requires (Abi::has_simd) {
    return fp_regs();
  }

  std::byte* shadow_space() const noexcept requires (shadow_bytes != 0) {
    return frame() + offsetof(typename Abi::bootstrap_type, shadow_space);
  }

  std::byte* red_zone() const noexcept requires (red_zone_bytes != 0) {
    return stack_top() - red_zone_bytes;
  }

  range_type aligned_range() const noexcept { return range_; }
  frame_range_type frame_range() const noexcept {
    return frame_range_type{frame()};
  }

 private:
  static std::byte* checked_data(std::byte* data) {
    if (data == nullptr ||
        (reinterpret_cast<std::uintptr_t>(data) & (alignment - 1)) != 0) {
      throw std::invalid_argument("meta_abi_range: storage must be nonnull and 16-byte aligned");
    }
    return data;
  }

  static std::byte* checked_storage(std::span<std::byte> storage) {
    if (storage.size() < N) {
      throw std::length_error("meta_abi_range: storage is smaller than N");
    }
    return storage.data();
  }

  range_type range_;
};

template <std::size_t N>
using meta_abi_range_win_x64 = meta_abi_range<N, win_x64>;
template <std::size_t N>
using meta_abi_range_sysv_x64 = meta_abi_range<N, sysv_x64>;
template <std::size_t N>
using meta_abi_range_win_x86 = meta_abi_range<N, win_x86>;
template <std::size_t N>
using meta_abi_range_sysv_x86 = meta_abi_range<N, sysv_x86>;
template <std::size_t N>
using meta_abi_range_aapcs64 = meta_abi_range<N, aapcs64>;
template <std::size_t N>
using meta_abi_range_aapcs32_vfp = meta_abi_range<N, aapcs32_vfp>;
template <std::size_t N>
using meta_abi_range_riscv64_lp64d = meta_abi_range<N, riscv64_lp64d>;
template <std::size_t N>
using meta_abi_range_riscv32_ilp32d = meta_abi_range<N, riscv32_ilp32d>;

}  // namespace meta_abi
