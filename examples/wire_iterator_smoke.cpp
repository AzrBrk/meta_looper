// meta_wire_iterator 冒烟测试：packed 布局 + 协议端序解码。
// 布局、读、写都放在 looper / pipe 语境里跑（不是在旁边直接 meta_invoke）。
//   g++ -std=c++23 -O0 -I. -o smoke examples/wire_iterator_smoke.cpp
#include "meta_ios.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;

namespace wire_tests {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// 协议布局：u8, u32, u16, double —— 全 packed；偏移 0 / 1 / 5 / 7，总长 15。
using wire_fields = meta_istream_list<u8, u32, u16, double>;

// ---- 1) pipe 语境：偏移表 ----
using wire_offsets = meta_pipe<wire_fields>
                         ::all_to<meta_wire_iterator<>>
                         ::run<meta_ostream<>>::type::to_t;
using packed_offsets = meta_pipe<wire_fields>
                           ::all_to<meta_aligned_iterator<1>>
                           ::run<meta_ostream<>>::type::to_t;

static constexpr auto wire_arr =
    exp_utilities::to_meta_array_t<wire_offsets>::array();
static constexpr auto packed_arr =
    exp_utilities::to_meta_array_t<packed_offsets>::array();

// 布局与机器端序无关：wire（大端）和 aligned<1>（packed）逐格相同。
static_assert(wire_arr == packed_arr);
static_assert(wire_arr == std::array<std::size_t, 4>{0, 1, 5, 7});

// packed 没有尾部 padding：总长就是最后一格的 advance。
static_assert(wire_offsets::template at<3>::advance_t::value == 15);

// 换协议端序也不改变任何偏移。
using wire_offsets_le =
    meta_pipe<wire_fields>::all_to<meta_wire_iterator<1, std::endian::little>>
        ::run<meta_ostream<>>::type::to_t;
static_assert(exp_utilities::to_meta_array_t<wire_offsets_le>::array() ==
              wire_arr);

// ---- 2) 同一帧的两种线格式（期望解出的值完全一样）----
inline constexpr std::array<std::byte, 15> be_frame{
    std::byte{0x07},
    std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
    std::byte{0x12}, std::byte{0x34},
    std::byte{0x40}, std::byte{0x09}, std::byte{0x21}, std::byte{0xFB},
    std::byte{0x54}, std::byte{0x44}, std::byte{0x2D}, std::byte{0x18},
};
inline constexpr std::array<std::byte, 15> le_frame{
    std::byte{0x07},
    std::byte{0xEF}, std::byte{0xBE}, std::byte{0xAD}, std::byte{0xDE},
    std::byte{0x34}, std::byte{0x12},
    std::byte{0x18}, std::byte{0x2D}, std::byte{0x44}, std::byte{0x54},
    std::byte{0xFB}, std::byte{0x21}, std::byte{0x09}, std::byte{0x40},
};

// ---- 3) looper 语境：逐格读（类型从 to_t 自己推导，不写模板参数）----
template <class Iter, class Frame>
constexpr bool reads_as_expected(Frame const& frame) {
  bool ok = true;
  meta_states::run<meta_ios::stream_states_co<>, Iter, wire_fields>::for_each(
      [&]<class Stream>(Stream s) {
        using cursor = typename Stream::to_t;   // wire_seek_to<偏移, 字段, ...>
        using field = typename cursor::type;
        auto const* base = frame.data();
        if constexpr (std::is_same_v<field, u8>) {
          ok = ok && s.object().read(base) == u8{0x07};
        } else if constexpr (std::is_same_v<field, u32>) {
          ok = ok && s.object().read(base) == u32{0xDEADBEEF};
        } else if constexpr (std::is_same_v<field, u16>) {
          ok = ok && s.object().read(base) == u16{0x1234};
        } else {
          static_assert(std::is_same_v<field, double>);
          // 3.141592653589793 的 IEEE-754 位模式，避免浮点相等比较。
          ok = ok &&
               std::bit_cast<std::uint64_t>(s.object().read(base)) ==
                   0x4009'21FB'5444'2D18ull;
        }
      });
  return ok;
}

// ---- 4) looper 语境：逐格写（for_each_forward 每轮转发一个参数）----
template <class Iter>
constexpr std::array<std::byte, 15> writes_frame() {
  std::array<std::byte, 15> out{};
  meta_states::run<meta_ios::stream_states_co<>, Iter, wire_fields>
      ::for_each_forward(
          [&]<class Stream, class V>(Stream s, V v) {
            s.object().write(out.data(), v);   // 隐式转成字段类型
          },
          u8{0x07}, u32{0xDEADBEEF}, u16{0x1234}, 3.141592653589793);
  return out;
}

// 大端协议：读/写都按大端，期望字节是固定的，因此本机端序无关。
static_assert(reads_as_expected<meta_wire_iterator<>>(be_frame));
static_assert(writes_frame<meta_wire_iterator<>>() == be_frame);

// 小端协议：同一套代码，只换模板参数。
using le_iter = meta_wire_iterator<1, std::endian::little>;
static_assert(reads_as_expected<le_iter>(le_frame));
static_assert(writes_frame<le_iter>() == le_frame);

// 交叉：大端读出来的值也等于小端读出来的值（同一帧语义）。
static_assert(reads_as_expected<meta_wire_iterator<>>(be_frame) ==
              reads_as_expected<le_iter>(le_frame));

}  // namespace wire_tests

int main() {
  // 非 constexpr 语境再走一遍，确认运行期可用。
  using wire_tests::be_frame;
  using wire_tests::le_frame;

  auto const be = wire_tests::writes_frame<meta_wire_iterator<>>();
  auto const le = wire_tests::writes_frame<
      meta_wire_iterator<1, std::endian::little>>();

  bool const ok = be == be_frame && le == le_frame &&
                  wire_tests::reads_as_expected<meta_wire_iterator<>>(be_frame) &&
                  wire_tests::reads_as_expected<
                      meta_wire_iterator<1, std::endian::little>>(le_frame);

  std::cout << (ok ? "wire iterator ok" : "wire iterator FAILED") << '\n';
  return ok ? 0 : 1;
}
