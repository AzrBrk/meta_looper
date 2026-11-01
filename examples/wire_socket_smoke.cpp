// 真实场景：Boost.Asio 回环 TCP。服务器发一帧 packed / 大端二进制帧，客户端用
// meta_wire_iterator 在 looper 语境里直接解码，改一个字段再编码回去，服务器再解
// 一遍验证。全程没有“把收到的字节 reinterpret 成协议结构体”。
//
// Windows（g++/MinGW）：
//   g++ -std=c++23 -O0 -I. -I G:/boost_1_84_0/boost_1_84_0 -o smoke
//       examples/wire_socket_smoke.cpp -lws2_32
// Linux：
//   g++ -std=c++23 -O0 -I. -I /path/to/boost -o smoke
//       examples/wire_socket_smoke.cpp -pthread
#include <boost/asio.hpp>

#include "type_safe_aligned_iterator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>

using namespace meta_states;
using namespace meta_iostream;
using namespace meta_ios;

namespace sensor {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// 协议：magic u32 / version u8 / kind u8 / flags u16 / device u32 /
//       timestamp u64 / value double / length u32 —— 全大端、packed。
using fields = meta_istream_list<u32, u8, u8, u16, u32, u64, double, u32>;

inline constexpr std::size_t frame_bytes = 32;

// 协议是 packed（32）；按 alignof 排会变成 40（u64/double 各自要 8 对齐）。
// 这就是“真实的 wire 布局”，range 不指定 Alignment=1 会算错。
static_assert(type_safe_aligned::field_size<1, u32, u8, u8, u16, u32, u64,
                                            double, u32>() == 32);
static_assert(type_safe_aligned::field_size<0, u32, u8, u8, u16, u32, u64,
                                            double, u32>() == 40);

// 解码结果不是一组值，也不是“套在缓冲区上的结构体”，而是**借用一段内存的异构
// 视图**：第 i 格就是协议第 i 个字段（packed 布局）。内存由调用方持有，view 只
// 记一个指针。
using frame_storage = std::array<std::byte, frame_bytes>;
using frame_view = type_safe_aligned::type_safe_aligned_range<
    1, u32, u8, u8, u16, u32, u64, double, u32>;

// 具名槽位：只是**序号**，不是字节偏移（偏移完全由 range 按 packed 布局算）。
inline constexpr std::size_t magic_slot = 0;
inline constexpr std::size_t version_slot = 1;
inline constexpr std::size_t kind_slot = 2;
inline constexpr std::size_t flags_slot = 3;
inline constexpr std::size_t device_slot = 4;
inline constexpr std::size_t timestamp_slot = 5;
inline constexpr std::size_t value_slot = 6;
inline constexpr std::size_t length_slot = 7;

// 索引是编译期常量，就直接拿那一格的游标（偏移也是编译期常量），运行时只是一次
// 指针加法——不要用运行期迭代器挪 6 步。
static_assert(type_safe_aligned::slot_cursor_t<magic_slot, frame_view>::value == 0);
static_assert(type_safe_aligned::slot_cursor_t<device_slot, frame_view>::value == 8);
static_assert(type_safe_aligned::slot_cursor_t<value_slot, frame_view>::value == 20);
static_assert(type_safe_aligned::slot_cursor_t<length_slot, frame_view>::value == 28);

// wire → 视图：wire cursor 负责偏移与端序，range 负责“这段内存怎么摆”；两边都按
// 协议顺序走，槽位一一对应，全程没有手写偏移。
void decode_into(frame_view view, std::byte const* wire) {
  auto it = view.begin();
  meta_states::run<meta_ios::stream_states_co<>, meta_wire_iterator<>,
                   fields>::for_each([&]<class Stream>(Stream s) {
    (*it).emplace(s.object().read(wire));  // 在第 i 格就地构造第 i 个字段
    ++it;
  });
}

// 视图 → wire：同样按协议顺序，从每格的引用里取值交给 wire cursor。
frame_storage encode(frame_view view) {
  frame_storage out{};
  auto it = view.begin();
  meta_states::run<meta_ios::stream_states_co<>, meta_wire_iterator<>,
                   fields>::for_each([&]<class Stream>(Stream s) {
    using field = typename Stream::to_t::type;
    s.object().write(out.data(), static_cast<field&>(*it));
    ++it;
  });
  return out;
}

// 造一帧（构造也用 range：initialize 按槽位顺序 emplace，没有偏移）。
frame_storage make_frame(double value) {
  frame_storage storage{};
  frame_view view{storage.data()};
  type_safe_aligned::initialize(
      view, u32{0xCAFE'BABEu}, u8{1}, u8{2}, u16{0}, u32{0x0000'0123u},
      u64{0x0000'0001'7090'0000ull}, value, u32{0});
  return encode(view);
}

// 服务器：accept → 发帧 → 收回包 → 直接用视图核对字节。
bool run_roundtrip() {
  using boost::asio::ip::tcp;

  auto const server_frame = make_frame(23.5);
  auto const expected_reply = make_frame(24.5);

  boost::asio::io_context ctx;
  tcp::acceptor acceptor(
      ctx, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
  auto const port = acceptor.local_endpoint().port();

  bool server_ok = false;
  std::jthread server{[&] {
    try {
      tcp::socket sock(ctx);
      acceptor.accept(sock);

      boost::asio::write(sock, boost::asio::buffer(server_frame));

      std::array<std::byte, frame_bytes> wire{};
      boost::asio::read(sock, boost::asio::buffer(wire));

      frame_storage storage{};
      frame_view view{storage.data()};
      decode_into(view, wire.data());

      std::cout << "server got: value="
                << type_safe_aligned::slot_at<value_slot>(view).get(
                       view.data())
                << " device=0x" << std::hex
                << type_safe_aligned::slot_at<device_slot>(view).get(
                       view.data())
                << std::dec << " timestamp="
                << type_safe_aligned::slot_at<timestamp_slot>(view).get(
                       view.data())
                << '\n';

      // 客户端只把 value 加了 1，整帧字节必须与预期一致。
      server_ok = wire == expected_reply;
    } catch (std::exception const& e) {
      std::cerr << "server error: " << e.what() << '\n';
    }
  }};

  tcp::socket client(ctx);
  client.connect(
      tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

  std::array<std::byte, frame_bytes> incoming{};
  boost::asio::read(client, boost::asio::buffer(incoming));

  frame_storage storage{};
  frame_view view{storage.data()};
  decode_into(view, incoming.data());

  std::cout << "client got: magic=0x" << std::hex
            << type_safe_aligned::slot_at<magic_slot>(view).get(view.data())
            << " device=0x"
            << type_safe_aligned::slot_at<device_slot>(view).get(view.data())
            << std::dec << " timestamp="
            << type_safe_aligned::slot_at<timestamp_slot>(view).get(
                   view.data())
            << " value="
            << type_safe_aligned::slot_at<value_slot>(view).get(view.data())
            << '\n';

  // 往返校验：视图 → wire 必须和服务器发出来的字节完全一致（所有字段一起验）。
  bool const client_ok = encode(view) == server_frame;

  // 只改一个字段（按槽位取游标，编译期偏移 + 一次指针加法）。
  type_safe_aligned::slot_at<value_slot>(view).get(view.data()) += 1.0;
  auto const reply = encode(view);
  boost::asio::write(client, boost::asio::buffer(reply));
  client.shutdown(tcp::socket::shutdown_both);
  client.close();

  server.join();
  return client_ok && server_ok;
}

}  // namespace sensor

int main() {
  try {
    bool const ok = sensor::run_roundtrip();
    std::cout << (ok ? "wire socket roundtrip ok" : "wire socket roundtrip FAILED")
              << '\n';
    return ok ? 0 : 1;
  } catch (std::exception const& e) {
    std::cerr << "client error: " << e.what() << '\n';
    return 1;
  }
}
