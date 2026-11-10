
#if defined(META_SOCKET_LOOPBACK_TEST)
#include <boost/asio.hpp>
#include <chrono>
#include <stdexcept>
#endif
#include <cassert>
#include <cstdint>
#include <bit>
#include <array>
#include <iostream>
#include "type_safe_aligned_iterator.hpp"
#include "meta_abi_frames.hpp"
#include "meta_execution_context.hpp"
#include<memory>

namespace abi_demo {

using namespace meta_abi;

// ① 布局常量：三种 ABI 的帧大小、槽位偏移都由 type_safe_aligned 算，
//    不写任何 sizeof/offsetof。at<I> 就是第 I 个槽相对起点的绝对偏移。
static_assert(frame_size<ret_address, stack_word, stack_word> == 24);
static_assert(aligned_offset_stream<0, ret_address, shadow_space, stack_word,
                                    stack_word>::template at<2>::value == 40);
static_assert(aligned_offset_stream<0, frame_record, callee_gprs,
                                    callee_simd>::template at<2>::value == 96);

void sizes() {
  std::cout << "sysv  entry    = " << frame_size<ret_address, stack_word, stack_word>
            << " bytes\n"
            << "win64 entry    = "
            << frame_size<ret_address, shadow_space, stack_word, stack_word>
            << " bytes\n"
            << "win64 context  = "
            << frame_size<ret_address, shadow_space, std::array<xmm_slot, 10>>
            << " bytes\n"
            << "arm64 tramp    = "
            << frame_size<frame_record, callee_gprs, callee_simd> << " bytes\n";
}

// ② 合成一个 Win64 入口帧：shadow space 之后（偏移 40）才是第一个栈参数。
void synthetic_win64() {
  using layout = win64_x86_64_entry<stack_word, stack_word>;
  alignas(16) std::byte
      buffer[frame_size<ret_address, shadow_space, stack_word, stack_word>]{};
  layout frame{buffer};
  type_safe_aligned::initialize(frame, ret_address{0xDEAD'BEEF'CAFE'0001ull},
                                shadow_space{}, stack_word{42}, stack_word{43});

  auto const& shadow = frame.get<slot_index(win64_x86_64_slot::shadow_space)>();
  auto const& arg0 = frame.get<slot_index(win64_x86_64_slot::arg0)>();
  std::cout << "shadow_space at +"
            << (reinterpret_cast<std::byte const*>(&shadow) - buffer)
            << ", arg0 at +"
            << (reinterpret_cast<std::byte const*>(&arg0) - buffer)
            << ", arg0=" << arg0.value << ", arg1="
            << frame.get<slot_index(win64_x86_64_slot::arg1)>().value << '\n';

  // 运行期下标 + 类型检查：第 1 格是 shadow_space，取成 stack_word 要被拒。
  auto slot = frame[slot_index(win64_x86_64_slot::shadow_space)];
  std::cout << "frame[1].safe<shadow_space>="
            << (slot.safe<shadow_space>() ? "true" : "false")
            << ", frame[1].safe<stack_word>="
            << (slot.safe<stack_word>() ? "true" : "false") << '\n';
  // 槽位都是 trivially destructible，不需要 destroy_range。
}

// ③ 真实返回地址：ABI 说入口帧的第 0 槽就是 [entry_rsp]，而
//    __builtin_return_address(0) 给的就是那个字。把它填进一块“入口帧”，
//    再用 frame_at + 布局读回来，两条路径都要等于它。
//    注意不要直接读 [__builtin_frame_address(0) + 8]：clang -O1 会省略帧指针，
//    那个地址不等于 rbp，[+8] 并不是返回地址（GCC 这里是碰巧成立）。
void real_return_address() {
#if defined(_WIN32)
  using entry_frame = win64_x86_64_entry<>;
  constexpr auto ret_slot = win64_x86_64_slot::ret_address;
#else
  using entry_frame = sysv_x86_64_entry<>;
  constexpr auto ret_slot = sysv_x86_64_slot::ret_address;
#endif
  const word ground_truth = reinterpret_cast<word>(__builtin_return_address(0));

  alignas(16) std::byte buffer[frame_size<ret_address>]{};
  ::new (static_cast<void*>(buffer)) ret_address{ground_truth};

  auto frame = frame_at<entry_frame>(buffer);
  const word via_layout = frame.get<slot_index(ret_slot)>().value;
  const word via_helper = return_address_of(buffer);
  std::cout << "ret addr: layout=0x" << std::hex << via_layout << " helper=0x"
            << via_helper << " builtin=0x" << ground_truth << std::dec
            << " equal="
            << ((via_layout == ground_truth && via_helper == ground_truth)
                    ? "true"
                    : "false")
            << '\n';
}

}  // namespace abi_demo

namespace socket_demo {

using socket_state = type_safe_aligned::type_safe_aligned_range<
    0, bool, bool, std::string>;
socket_state* active_socket = nullptr;

std::string read_socket(socket_state& socket) {
  while (!socket.get<0>()) {
    socket.get<1>() = true;
    std::cout << "reader: would_block -> yield\n";
    meta_execution::yield();
  }
  socket.get<1>() = false;
  socket.get<0>() = false;
  return std::move(socket.get<2>());
}

void reader_entry(meta_execution::fiber&) {
  const std::string request = "request-42";
  auto* request_address = &request;
  const auto message = read_socket(*active_socket);
  assert(request == "request-42" && &request == request_address);
  assert(message == "hello from socket");
  std::cout << "reader: resumed, " << request << ", received: " << message
            << '\n';
}

void run() {
  alignas(std::max_align_t) std::byte state_buffer[
      type_safe_aligned::field_size<0, bool, bool, std::string>()];
  socket_state socket{state_buffer};
  type_safe_aligned::initialize(socket, false, false, std::string{});
  active_socket = &socket;

  alignas(64) std::byte stack_memory[128 * 1024];
  meta_execution::stack_arena arena{stack_memory, sizeof(stack_memory)};
  meta_execution::fiber reader{arena, 64 * 1024, reader_entry};

  std::cout << "---------- simulated socket wait (existing backend) ----------\n";
  const bool suspended = reader.resume();
  assert(suspended && socket.get<1>() && !reader.finished());

  for (int tick = 1; tick <= 3; ++tick) {
    std::cout << "event loop: tick " << tick << ", doing other work\n";
    assert(socket.get<1>() && !reader.finished());
    if (tick == 3) {
      socket.get<2>() = "hello from socket";
      socket.get<0>() = true;
      std::cout << "event loop: socket readable -> resume reader\n";
      const bool still_running = reader.resume();
      assert(!still_running && reader.finished() && !socket.get<1>());
      (void)still_running;
    }
  }
  (void)suspended;
  assert(reader.finished());
  active_socket = nullptr;
  type_safe_aligned::destroy_range(socket);
}

#if defined(META_SOCKET_LOOPBACK_TEST)
void run_loopback() {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  using clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  asio::io_context events;
  tcp::acceptor listener{events, {asio::ip::address_v4::loopback(), 0}};
  tcp::socket sender{events};
  sender.connect(listener.local_endpoint());
  tcp::socket receiver = listener.accept();
  const std::string payload = "hello from socket";
  std::string received(payload.size(), '\0');

  alignas(std::max_align_t) std::byte state_buffer[
      type_safe_aligned::field_size<0, bool, bool, std::string>()];
  socket_state socket{state_buffer};
  type_safe_aligned::initialize(socket, false, false, std::string{});
  active_socket = &socket;
  alignas(64) std::byte stack_memory[128 * 1024];
  meta_execution::stack_arena arena{stack_memory, sizeof(stack_memory)};
  meta_execution::fiber reader{arena, 64 * 1024, reader_entry};

  bool completed = false;
  bool sent = false;
  bool timed_out = false;
  int other_work = 0;
  boost::system::error_code read_error;
  boost::system::error_code write_error;
  asio::steady_timer delivery{events, 150ms};
  asio::steady_timer deadline{events, 2s};
  std::array<asio::steady_timer, 3> work{
      asio::steady_timer{events, 25ms},
      asio::steady_timer{events, 50ms},
      asio::steady_timer{events, 75ms}};

  std::cout << "---------- real TCP loopback: delayed send 150 ms ----------\n";
  const auto started = clock::now();
  const bool suspended = reader.resume();
  assert(suspended && socket.get<1>() && !reader.finished());
  (void)suspended;

  asio::async_read(receiver, asio::buffer(received),
      [&](boost::system::error_code error, std::size_t bytes) {
        read_error = error;
        completed = true;
        if (!error && bytes == payload.size()) {
          socket.get<2>() = received;
          socket.get<0>() = true;
        }
        deadline.cancel();
      });
  delivery.async_wait([&](boost::system::error_code error) {
    if (error) return;
    sent = true;
    asio::async_write(sender, asio::buffer(payload),
        [&](boost::system::error_code failure, std::size_t) {
          write_error = failure;
        });
  });
  for (auto& timer : work) {
    timer.async_wait([&](boost::system::error_code error) {
      if (error) return;
      assert(!completed && !reader.finished() && socket.get<1>());
      ++other_work;
      std::cout << "event loop: other work " << other_work << '\n';
    });
  }
  deadline.async_wait([&](boost::system::error_code error) {
    if (error) return;
    timed_out = true;
    receiver.cancel();
    sender.cancel();
    delivery.cancel();
  });

  events.run();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      clock::now() - started).count();
  const bool valid = completed && sent && !timed_out && !read_error &&
      !write_error && received == payload && other_work == 3 && elapsed >= 150;
  if (!valid) {
    active_socket = nullptr;
    type_safe_aligned::destroy_range(socket);
    throw std::runtime_error("loopback delay validation failed");
  }
  assert(!reader.finished() && socket.get<1>());
  const bool still_running = reader.resume();
  assert(!still_running && reader.finished() && !socket.get<1>());
  (void)still_running;
  active_socket = nullptr;
  type_safe_aligned::destroy_range(socket);
  std::cout << "loopback: PASS, elapsed=" << elapsed
            << " ms, other_work=" << other_work << '\n';
}
#endif

}  // namespace socket_demo


int main() {
   
   constexpr auto f_size = type_safe_aligned::field_size<0, int, double, char, double, std::string>();

   std::unique_ptr<std::byte[]> arena{new std::byte[f_size]};

   std::byte * ptr = arena.get();
   

   type_safe_aligned::type_safe_aligned_range<0, int,double, char, double, std::string> range{ptr};

   type_safe_aligned::initialize(range, 10,2.33,'k', 3.14, "hello");

   std::size_t i = 3;
   double opl = 1.4;

   uint64_t op{0};

   // double 不在通用寄存器里：输入约束必须写 "x"（xmm），movq %xmm, %r64 才成立。
   // 写 "r" 会报 inconsistent operand constraints。
   __asm__ volatile("movq %1, %0"
        :"=r"(op)
        :"x"(opl)
    );

   // 1) 位模式经过 GPR 往返是精确的：op 就是 1.4 的 IEEE-754 编码，一个 bit 都没丢。
   std::cout << std::boolalpha
             << "op = " << op
             << ", bit_cast<double>(op) == 1.4 : "
             << (std::bit_cast<double>(op) == opl) << std::endl;

   // 2) 操作数通道只有两种解释：std::uint64_t = 精确整数，double = 浮点。
   //    把 1.4 的位模式当“整数数值”加进去，算出来就是 3.14 + 4.6e18。
   range[i] += op;
   std::cout << "after range[3] += op : " << static_cast<double>(range[3]) << std::endl;

   // 3) 浮点增量直接走 double 通道，不需要再手动读-改-写。
   range[3] = 3.14;
   range[3] += 1.4;
   std::cout << "after range[3] += 1.4 : " << static_cast<double>(range[3]) << std::endl;
   double val = range[3];

   std::cout << "range[3] = " << val << std::endl;

   for(auto const& i : range){
    std::cout << i << std::endl;
   }
   for(auto riter = range.rbegin(); riter!= range.rend(); ++riter){
    std::cout << *riter << std::endl;
   }
   type_safe_aligned::destroy_range(range);

   std::cout << "---------- meta_abi_frames ----------" << std::endl;
   abi_demo::sizes();
   abi_demo::synthetic_win64();
   abi_demo::real_return_address();
  socket_demo::run();
#if defined(META_SOCKET_LOOPBACK_TEST)
  socket_demo::run_loopback();
#endif
}