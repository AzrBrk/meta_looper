#pragma once

// ============================================================================
// meta_states —— meta_states_looper 的公开层（API 门面）
//
// 这一层只负责"把该开放的东西摆整齐 + 提供内置组件"，不含任何机制实现；
// 机制都在 meta_states_looper.hpp（impl 命名空间）里，用户不需要碰它。
//
// 一轮循环 = 四个监控窗口，围着"生成"和"接收"两次执行：
//
//     ① pre_gen_exec   拿 Generator 调条件   → 要不要继续(G_CONTINUE)、要不要拉(G_ADVANCE)
//     ② post_gen_exec  拿推进后的 Generator  → 拉到的东西怎么算
//     ③ pre_rec_exec   拿 Receiver 调条件    → 这一步收不收(G_RECEIVE)、回调看什么
//     ④ post_rec_exec  拿接收后的 Receiver   → 记账(G_FINISH)、下一轮怎么走
//
// 用户只需要写窗口函数（一个按窗口号分派的模板）+ 一个 Receiver（+ 可选 Generator）。
// 阶段号推进、index 记账、计时器递减、压制、以及"G_FINISH 掐掉本轮后面的窗口"
// 全部由 looper 自己的 flags 控制（user_flags_control）完成。
//
// ---------------------------------------------------------------------------
// 最小用法：
//
//   template <u8 stage>                       // 窗口号就是特化参数，名字随你
//   struct my_exec : meta_states::ret_origin<stage> {};   // 没写的窗口 → 直通
//
//   template <>                               // 窗口①：拿生成器
//   struct my_exec<meta_states::pre_gen_exec> {
//     template <class flags, class gen>
//     struct apply { static constexpr auto value = []{ ... }(); };
//   };
//
//   using cond = meta_states::make_condition<
//       meta_states::states_executor<my_exec>>;
//   meta_states::run<cond, meta_states::recv_t, gen_t>::for_each([](auto s){ ... });
//
// 或者只写一个模板、用 apply 的偏特化挑窗口（见 exactly_at 那种写法）。
// 重复特化同一个窗口是重定义错误；窗口号拼错也是普通编译错误。
// ---------------------------------------------------------------------------

#include "meta_states_looper.hpp"

namespace meta_states {

// ===== 0) 常用别名（免得用户自己再找命名空间）=====
namespace bits = meta_loop::meta_states_looper_bits;
using bits::u8;
using bits::u16;
using bits::u64;
using bits::u128;
using meta_objects::flags_operator;
using meta_objects::meta_empty_o;
using meta_objects::meta_object;
using meta_objects::meta_states_object;

// ===== 1) 控制位（looper 认的 8 位，全在 flags 低 64 位的最高字节）=====
//
// 语义都按"置位 = 做这件事"读，只有 G_FINISH 是负位（置位 = 本轮到此为止）：
//   G_CONTINUE  总闸：循环是否继续（也是其它门的前提）
//   G_NOT_SKIP  这一步要不要回调用户
//   G_ADVANCE   生成器要不要推进
//   G_RECEIVE   接收器要不要接收
//   G_FINISH    本轮记账/到此为止：掐掉本轮后面的窗口 + 阶段号复位（一次性，结转时清掉）
//   G_FORWARD   传输方向：置位 = Gen→Rec；清掉 = 下一阶段把两者位置互换
//   G_PACK      回调看到"打包视图"（优先于 G_FEED_GEN）
//   G_FEED_GEN  回调看到生成器的产出（cache）
using bits::G_CONTINUE;
using bits::G_NOT_SKIP;
using bits::G_ADVANCE;
using bits::G_RECEIVE;
using bits::G_FINISH;
using bits::G_FORWARD;
using bits::G_PACK;
using bits::G_FEED_GEN;

// 默认门：全开（继续 / 回调 / 推进 / 接收 / 保持方向 / 打包），G_FINISH 关。
inline constexpr u64 default_gates = meta_loop::meta_states_looper_default;

// ===== 2) 四个阶段（窗口）+ 阶段镜像的位置 =====
using meta_states_looper_details::meta_states_condition_funtion_detail::pre_gen_exec;
using meta_states_looper_details::meta_states_condition_funtion_detail::post_gen_exec;
using meta_states_looper_details::meta_states_condition_funtion_detail::pre_rec_exec;
using meta_states_looper_details::meta_states_condition_funtion_detail::post_rec_exec;
// 阶段镜像写在 user 字段的高 4 位（stage_index_mask 是它的掩码）；
// user 字段的低 4 位留给用户自己用，looper 写回时会原样保留。
using meta_states_looper_details::meta_states_condition_funtion_detail::stage_index_mask;
using meta_states_looper_details::meta_states_condition_funtion_detail::user_stage_shift;

// ===== 3) flags 的字段布局（想自己拼 flags 的进阶用户用）=====
using bits::position_shift;   // u16：已完整走完的轮数（轮尾 +1；同一轮四个窗口看到的值相同）
using bits::user_shift;       // u8 ：高 4 位阶段镜像 / 低 4 位用户自用
using bits::timer0_shift;     // u16：计时器槽 0
using bits::timer1_shift;     // u16：计时器槽 1
using bits::gates_shift;      // u8 ：8 个控制位
using bits::position_mask;
using bits::user_mask;
using bits::timer0_mask;
using bits::timer1_mask;
using bits::gates_mask;
// looper 会反应的位集合（就是 gates 字节）+ "一个计时器只能压一个 looper 位"的约束
using bits::looper_controlled_mask;
using bits::looper_controlled_bit;

// ===== 4) 计时器 =====
//
// 计时单位是"轮"：looper 在每轮结束（用户打了 G_FINISH，或走到窗口④）递减一次，
// 计数减到 0 时整个字段被清空 —— 所以 timer_value == 0 真正等于"这个槽是空的"。
// 压制：计数 > 0 时目标位在每次窗口写回时被清掉（用户函数再把它打开也会被压住）。
using bits::set_suppression_timer;       // 同槽还在计时时不覆盖
using bits::override_suppression_timer;  // 强行换表
using bits::clear_timer;
using bits::tick_timers;
using bits::timer_value;     // 整个字段 = (计数 << 6) | 目标位下标
using bits::timer_count;
using bits::timer_target;
using bits::timer_max_count;
using bits::effective;       // 按当前计时器压掉目标位后的位图

// ===== 5) 条件对象的起始 flags =====
// 默认门 + 阶段镜像指向窗口①（一轮从这里开始）。
inline constexpr u64 condition_seed =
    meta_loop::meta_states_looper_default | (u64(pre_gen_exec) << user_shift);

// ===== 6) 内置组件：窗口函数 + 条件函数 =====
//
// 用户的"窗口函数"就是一个按窗口号分派的东西，两种写法都行，用的都是 C++ 自己的规则：
//
//   A) 按窗口号特化的模板（没写的窗口继承 ret_origin 直通）
//      template <u8 stage>
//      struct my_exec : ret_origin<stage> {};
//      template <>
//      struct my_exec<pre_gen_exec> {
//        template <this_ref flags, class gen>
//        struct apply { static constexpr auto value = ...; };
//      };
//      using cond = make_condition<states_executor<my_exec>>;   // 按 stage 取表项
//
//   B) 一个模板 + apply 的偏特化（只改自己想改的窗口）
//      template <std::size_t I>
//      struct exactly_at {
//        template <class stage_t, class flags, class obj>
//        struct apply { ... };                                                        // 通用
//        template <class flags, class obj>
//        struct apply<std::integral_constant<u8, pre_rec_exec>, flags, obj> { ... };   // 只改窗口③
//      };
//      using cond = make_condition<exactly_at<2>>;
//
// 库这边不需要检查"写了几个 / 有没有写重"：同一个窗口特化两次，C++ 自己就报重定义；
// 窗口号写成 pre_gen_exec 这些名字，拼错标识符也是普通编译错误。

using meta_objects::this_ref;
template<u8 any_flags>
struct ret_origin{
    template<this_ref flags, class obj>
    using apply = flags;
};

// 6.2) 内置条件函数：按 looper 维护的阶段镜像（flags 里 user 字段的高 4 位）取出
//      当前窗口号，把它当作"键"连同 flags / obj 一起交给 executor。
//      executor 可以是 states_executor<Fn>（按 stage 特化），也可以直接是带 apply 的模板。
//      只读高 4 位（掩掉低 4 位），所以用户可以在低 4 位放自己的东西。
template <class Executor>
struct state_fn {
    template <class this_flags, class obj>
    struct apply {
        static constexpr u8 stage_index = static_cast<u8>(
            this_flags::template get<u8, user_shift> & stage_index_mask);
        static constexpr auto value =
            meta_invoke<Executor, std::integral_constant<u8, stage_index>,
                        this_flags, obj>::value;
    };
};

// 6.3) 把 executor 包成 looper 需要的条件元对象（meta_states_object）。
// Flags 可以换一套起始门 / position，默认就是 condition_seed；
// 执行时的阶段镜像由调度器在每个窗口调用前写入，用户不能用它跳转窗口。
template <class Executor, u128 Flags = condition_seed>
using make_condition = meta_states_object<Flags, state_fn<Executor>>;

// ===== 7) 循环入口 + 视图 =====

// 引擎：::apply（带参数跑一步）/ ::type / ::for_each / ::for_each_forward
template <class Condition, class Receiver, class Generator = meta_empty_o>
using looper = meta_states_looper_details::meta_states_looper<Condition, Receiver, Generator>;

// 自定义 driver policy 的入口（policy 需要提供 round/more/done/decide 窗口）。
template <class Condition, class Receiver, class Generator = meta_empty_o,
          class DriverPolicy = meta_states_looper_details::
              default_driver_policy<meta_states_looper_details::meta_states_looper_budget>>
using looper_with_policy = meta_states_looper_details::meta_states_looper<
    Condition, Receiver, Generator, DriverPolicy>;

// 跑完之后的最终类型（就是 run<...>::type）
template <class Condition, class Receiver, class Generator = meta_empty_o,
          class... Args>
using looper_t = meta_states_looper_details::meta_states_looper_t<
    Condition, Receiver, Generator, Args...>;

// 推荐的运行入口：run<cond, recv, gen>::for_each(f) / ::for_each_forward(f, args...)
// Generator 默认 meta_empty_o（没有生成器时每轮产出空标签，见 meta_empty_o 的语义）。
template <class Condition, class Receiver, class Generator = meta_empty_o,
          class... Args>
using run = meta_states_looper_details::meta_states_for<
    Condition, Receiver, Generator, Args...>;

template <class Condition, class Receiver, class Generator,
          class DriverPolicy, class... Args>
using run_with_policy = meta_states_looper_details::meta_states_for_with_policy<
    Condition, Receiver, Generator, DriverPolicy, Args...>;

// 打包视图（G_PACK 置位时回调 / 终态看到的东西）：现算的投影，不持有状态。
//   ::to_t / ::from_t / ::cache / ::states（flags 类型）
//   .index() / .value() / .left() / .object()
template <class Receiver, class Generator, class States>
using pack_stream =
    meta_states_looper_details::meta_pack_stream<Receiver, Generator, States>;

namespace user_fn{
    using argument_stack = flags_operator<0>;
using meta_objects::flags_operator_type;
template<flags_operator_type Flags>
consteval std::size_t index_of(Flags = {}){
    return static_cast<std::size_t>(Flags::template get<u16, position_shift>);
}

// ===== 压制计时器的自动分槽 =====
// 两个槽（0 / 1）的"空闲"就是 count == 0（见 bits::tick：计数归零时整槽清空，
// 所以 timer_value == 0 真正等于空槽）。
// 2 表示两个槽都在计时，没有可自动分配的槽。
template<flags_operator_type Flags>
inline constexpr std::size_t first_free_timer_slot =
    bits::timer_count<Flags, 0> == 0 ? 0 :
    bits::timer_count<Flags, 1> == 0 ? 1 : 2;

// 装一张压制表：让 Bit 被压制 Times 轮，槽位按 Flags 当前的占用情况自动选，
// 用户不再写 Slot。
//   - Times == 0 → 不装（方便写成"条件压制"）；
//   - 已经有槽在压同一个 Bit → 原样返回（沿用 set_suppression_timer 的
//     "不覆盖正在走的表"语义，所以每轮重复调用也安全）；
//   - 两个槽都被别的目标占着 → static_assert 报错；等一个跑完，或显式用
//     override_suppression_timer 指定槽位。
// Bit / Times 走模板参数而不是函数参数：函数参数不是常量表达式，static_assert 用不了。
template<flags_operator_type Flags, u64 Bit, std::size_t Times>
consteval u128 set_suppression() {
    static_assert(std::has_single_bit(Bit) &&
                  (Bit & bits::looper_controlled_mask) == Bit,
                  "set_suppression: Bit must be a single looper-controlled bit");
    static_assert(Times <= bits::timer_max_count,
                  "set_suppression: Times must fit in the 10-bit timer count");
    constexpr bool same_target =
        (bits::timer_count<Flags, 0> != 0 &&
         bits::timer_target<Flags, 0> == Bit) ||
        (bits::timer_count<Flags, 1> != 0 &&
         bits::timer_target<Flags, 1> == Bit);
    constexpr std::size_t slot = first_free_timer_slot<Flags>;
    if constexpr (Times == 0 || same_target) {
        return Flags::value;
    } else if constexpr (slot > 1) {
        static_assert(slot < 2,
                      "set_suppression: both suppression timers are running; "
                      "wait for one to expire or use override_suppression_timer");
        return Flags::value;
    } else {
        constexpr u16 timer = static_cast<u16>(
            (Times << bits::timer_count_shift) | std::countr_zero(Bit));
        return bits::details::arm_timer_value(
            Flags::value, bits::details::timer_shift<slot>, timer, true);
    }
}
}

// 窗口/条件函数可以返回的决定标记：
//   skip_t  : 本轮不收/不发，循环继续
//   yield_t : 本轮收/发，循环继续
//   stop_t  : 结束循环
// 也可以写 skip() / yield() / stop()（constexpr 函数形式）。
using meta_states_control::skip_t;
using meta_states_control::yield_t;
using meta_states_control::stop_t;
using meta_states_control::skip;
using meta_states_control::yield;
using meta_states_control::stop;

}  // namespace meta_states
