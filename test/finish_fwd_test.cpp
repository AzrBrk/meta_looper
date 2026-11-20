#include "meta_stream64.hpp"
#include "meta_loop64.hpp"
#include "meta_iostream.hpp"

#include <type_traits>

using namespace meta_core64;
using namespace meta_loop64;
using namespace meta_iostream;
using namespace meta_stream64;

// ---------- G_FORWARD：pre_rec 关掉方向位，下一轮 Gen/Rec 角色互换 ----------
struct no_fwd {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = ops::yield(U);
    if constexpr (W == 2) w = ops::set_gates(w, 0, gates::fwd);
    return w;
  }();
};

using gen2 = index_sequence_istream<2>;
using S_fwd = state<lw::make(gates::defaults, 0, 0, 0, 0), 0, meta_iterator,
                    gen2, stream_cond64<no_fwd>>;

// gen 推进后的类型
using gen_advanced = meta_invoke<gen2>;

// 这个条件每轮都清 fwd，所以第 0 轮就互换：
//   next::rec = 本轮的 gen，next::gen = 本轮的 rec
// 第 0 轮是 generator delay（GEN 不推进），所以 gen1 仍是 gen2 本身。
using st0 = step<S_fwd>;
static_assert(st0::swap_);   // fwd 被清，且本轮继续
static_assert(std::is_same_v<typename st0::next::rec, typename st0::gen1>);
static_assert(std::is_same_v<typename st0::next::gen, typename st0::rec1>);
static_assert(std::is_same_v<typename st0::gen1, gen2>);

// ---------- G_FORWARD 一次性：下一轮状态里方向位被清掉 ----------
static_assert((lw::gates_v(st0::next::L) & gates::fwd) == 0);

// ---------- G_FINISH：pre_gen 置位，本轮到此为止 ----------
struct finish_at_pre_gen {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = ops::yield(U);
    if constexpr (W == 0) {
      if (lw::pos_v(L) == 0) w = ops::set_gates(w, gates::finish, 0);
    }
    return w;
  }();
};

using S_fin = state<lw::make(gates::defaults, 0, 0, 0, 0), 0, meta_iterator,
                    gen2, stream_cond64<finish_at_pre_gen>>;
using stf = step<S_fin>;

static_assert(stf::fin0);
static_assert(!stf::enter1);           // 后续窗口不再进入
static_assert(std::is_same_v<typename stf::next::gen, gen2>);       // 不推进
static_assert(std::is_same_v<typename stf::next::rec, meta_iterator>); // 不接收
static_assert(lw::pos_v(stf::next::L) == 1);   // 只记账一次
static_assert((lw::gates_v(stf::next::L) & gates::finish) == 0);  // 一次性

// ---------- G_FINISH 在 post_rec 置位：本来就到轮尾 ----------
struct finish_at_post_rec {
  template <u8 W, u64 L, u64 U, class Obj>
  static constexpr u64 value = [] {
    u64 w = ops::yield(U);
    if constexpr (W == 3) w = ops::set_gates(w, gates::finish, 0);
    return w;
  }();
};

using S_fin3 = state<lw::make(gates::defaults, 0, 0, 0, 0), 0, meta_iterator,
                     gen2, stream_cond64<finish_at_post_rec>>;
using stf3 = step<S_fin3>;
static_assert(!stf3::fin0 && !stf3::fin1 && !stf3::fin2);
static_assert(lw::pos_v(stf3::next::L) == 1);
static_assert((lw::gates_v(stf3::next::L) & gates::finish) == 0);

int main() { return 0; }
