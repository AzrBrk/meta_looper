#pragma once

#include "meta_objects.hpp"

// 每一段（chunk）最多跑多少轮。超过这个轮数就停成一个“可续跑状态”，
// 由外层 driver 重新开一段，从而避免单条递归链把模板实例化深度吃满。
// 可以在包含本头文件之前 -DMETA_STATES_LOOPER_BUDGET=... 覆盖。
#ifndef META_STATES_LOOPER_BUDGET
#define META_STATES_LOOPER_BUDGET 64
#endif

// 用户函数可以返回的“决定”标记。引擎会以当前 flags 为底，把它们归一化成
// 真正的 flags（而不是把标记本身当 flags 用）：
//   skip  : 本轮不收/不发，但循环继续（不碰 G_CONTINUE）
//   yield : 本轮收/发（置 G_RECEIVE / PIPE_EMIT），循环继续
//   stop  : 结束循环（清 G_CONTINUE）
// 类型形式（yield_t 等）用于 TMP 的 `using apply = ...`；
// 函数形式（yield() 等）方便 constexpr 代码和 `decltype(...)`。
namespace meta_states_control {
struct skip_t {};
struct yield_t {};
struct stop_t {};

constexpr skip_t skip() noexcept { return {}; }
constexpr yield_t yield() noexcept { return {}; }
constexpr stop_t stop() noexcept { return {}; }
}  // namespace meta_states_control

namespace meta_loop {
namespace meta_states_looper_bits {
using meta_objects::flags_operator;
using meta_objects::flags_operator_type;
using meta_objects::meta_op_bits::u8;
using meta_objects::meta_op_bits::u16;
using meta_objects::meta_op_bits::u64;
using meta_objects::meta_op_bits::u128;

//these bits are recognized by looper
inline constexpr u64 G_CONTINUE = u64{1} << 63;//总闸：循环是否继续（也是所有门的前提）
inline constexpr u64 G_NOT_SKIP = u64{1} << 62;//control if the user callable will be invoke
inline constexpr u64 G_FEED_GEN = u64{1} << 61;//control which part feed to the user callable
inline constexpr u64 G_PACK = u64{1} << 60;//control if result packed feed to the user callable, if this bit on, ignore G_FEED_GEN
inline constexpr u64 G_FORWARD = u64{1} << 59;//control if the loop flow forward(Gen->Rec), if off(Rec->Gen)
inline constexpr u64 G_FINISH = u64{1} << 58;//this round is accounted: bump the flags index (per-round only, consumed at settle), decide if index inc
inline constexpr u64 G_RECEIVE = u64{1} << 57;//control if the Receiver receives from the Generator
inline constexpr u64 G_ADVANCE = u64{1} << 56;//control if the Generator advances 

//for timer control, the following field are handled only by the flags_operator
//and not recognized by the looper
inline constexpr std::size_t position_shift = 0;
inline constexpr std::size_t user_shift = 16;
inline constexpr std::size_t timer0_shift = 24;
inline constexpr std::size_t timer1_shift = 40;
inline constexpr std::size_t gates_shift = 56;
inline constexpr std::size_t timer_count_shift = 6;
inline constexpr u16 timer_target_mask = 63;
inline constexpr std::size_t timer_max_count = 1023;
inline constexpr u64 position_mask = static_cast<u64>(flags_operator<0>::mask<u16, position_shift>);
inline constexpr u64 user_mask = static_cast<u64>(flags_operator<0>::mask<u8, user_shift>);
inline constexpr u64 timer0_mask = static_cast<u64>(flags_operator<0>::mask<u16, timer0_shift>);
inline constexpr u64 timer1_mask = static_cast<u64>(flags_operator<0>::mask<u16, timer1_shift>);
inline constexpr u64 gates_mask = static_cast<u64>(flags_operator<0>::mask<u8, gates_shift>);

// The set of bits the looper itself reacts to. By construction it is exactly
// the gates byte (bits 56..63); everything below is a position/user/timer
// field that only the flags_operator understands.
inline constexpr u64 looper_controlled_mask =
    G_CONTINUE | G_NOT_SKIP | G_FEED_GEN | G_PACK | G_FORWARD | G_FINISH |
    G_RECEIVE | G_ADVANCE;

// A timer may suppress exactly one looper-controlled bit.
// Single bit is required because the target is stored as a bit *index*:
// (timer >> timer_count_shift) = count, (timer & timer_target_mask) = index.
// That 6-bit field covers indices 0..63, so even G_CONTINUE (bit 63) fits.
template<u64 Target>
concept looper_controlled_bit =
    std::has_single_bit(Target) &&
    (Target & looper_controlled_mask) == Target;

namespace details {
template<std::size_t Slot> requires (Slot < 2)
inline constexpr std::size_t timer_shift = Slot == 0 ? timer0_shift : timer1_shift;

constexpr u64 suppression(u16 timer) noexcept {
  return (timer >> timer_count_shift) == 0
      ? u64{0} : u64{1} << (timer & timer_target_mask);
}

constexpr u16 tick(u16 timer) noexcept {
  // 计数 <= 1 时整个字段清空：
  //   - 计数 1 减完就归 0，槽位空闲，不能再把目标位下标留在里面（残留值无法区分
  //     "空槽"和"用过"，用户以后想自己写这个字段也会被污染）；
  //   - 计数已经是 0 的字段也顺手清一下，保证 timer_value == 0 真正等于"空槽"。
  return (timer >> timer_count_shift) <= 1
      ? u16{0} : static_cast<u16>(timer - (u16{1} << timer_count_shift));
}

constexpr u128 effective_value(u128 flags) noexcept {
  return flags & ~u128{
      suppression(static_cast<u16>(flags.low >> timer0_shift)) |
      suppression(static_cast<u16>(flags.low >> timer1_shift))};
}

constexpr u128 tick_value(u128 flags) noexcept {
  flags.low = (flags.low & ~(timer0_mask | timer1_mask)) |
      (u64{tick(static_cast<u16>(flags.low >> timer0_shift))} << timer0_shift) |
      (u64{tick(static_cast<u16>(flags.low >> timer1_shift))} << timer1_shift);
  return flags;
}

constexpr u128 count_round(u128 flags) noexcept {
  static_assert(position_shift == 0);
  flags.low = (flags.low & ~position_mask) |
      ((flags.low + 1) & position_mask);
  return tick_value(flags);
}

constexpr u128 arm_timer_value(u128 flags, std::size_t shift,
                               u16 timer, bool overwrite) noexcept {
  if (overwrite ||
      (static_cast<u16>(flags.low >> shift) >> timer_count_shift) == 0) {
    flags.low = (flags.low & ~(u64{0xFFFF} << shift)) |
                (u64{timer} << shift);
  }
  return flags;
}
}  // namespace details

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
inline constexpr u16 timer_value =
    Flags::template get<u16, details::timer_shift<Slot>>;

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
inline constexpr std::size_t timer_count = timer_value<Flags, Slot> >> timer_count_shift;

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
inline constexpr u64 timer_target =
    u64{1} << (timer_value<Flags, Slot> & timer_target_mask);

// Timers only suppress looper-controlled bits; the requested flags stay intact.
template<flags_operator_type Flags>
using effective = flags_operator<details::effective_value(Flags::value)>;

// 装表：同一个槽位**还在计时**（计数 != 0）时不覆盖，原样返回 Flags。
// 要强行换表用 override_suppression_timer。
// Count 必须 >= 1 —— 计时器只有"正在计时"一种有效状态，
// 写 count = 0 只会留下一个没意义的残字段（要清表用 clear_timer）。
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using set_suppression_timer = flags_operator<details::arm_timer_value(
    Flags::value, details::timer_shift<Slot>,
    static_cast<u16>((Count << timer_count_shift) | std::countr_zero(Target)), false)>;

// 强行覆盖同一个槽上正在走的计时器（无视它还在计时）。
// 想换目标位、改计数、或者把已经在走的表推倒重来，用这个。
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using override_suppression_timer = flags_operator<details::arm_timer_value(
    Flags::value, details::timer_shift<Slot>,
    static_cast<u16>((Count << timer_count_shift) | std::countr_zero(Target)), true)>;

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
using clear_timer = typename Flags::template deact<u16, details::timer_shift<Slot>>;

template<flags_operator_type Flags>
using tick_timers = flags_operator<details::tick_value(Flags::value)>;
}  // namespace meta_states_looper_bits

inline constexpr meta_objects::u64 meta_states_looper_default =
    meta_states_looper_bits::G_CONTINUE | meta_states_looper_bits::G_NOT_SKIP |
    meta_states_looper_bits::G_PACK | meta_states_looper_bits::G_FORWARD |
    meta_states_looper_bits::G_RECEIVE | meta_states_looper_bits::G_ADVANCE;

using meta_states_looper_flags = flags_operator<meta_states_looper_default>;
}  // namespace meta_loop

namespace meta_states_looper_details{
    //for convinience of developing
    //let's later delete the directly use of namespace
    using namespace meta_loop;
    using namespace meta_objects;
    using namespace exp_utilities;
    using namespace literal_types;
    using namespace meta_quote;
    using namespace meta_states_looper_bits;
    using namespace meta_invoke_protocols;

#include<typeinfo>
    template<
        meta_object_t Receiver, 
        meta_object_t Generator,
        flags_operator_type Control,
        class Driver = void>
    struct meta_pack_stream{
        using from   = Generator;
        using to     = Receiver;
        using to_t   = get_type<to>;
        using from_t = get_type<from>;
        using cache  = meta_produce_t<Generator>;
        using states = Control;
        using driver = Driver;

        consteval std::size_t index(){
            // position 在轮尾（窗口④ / G_FINISH）才 +1，而这份视图是轮尾之后建好的，
            // 所以减 1 才是"本轮的编号"；这正好等于本轮四个窗口里用户函数看到的 position。
            return states::template get<u16, position_shift> - 1;
        }
        consteval const std::type_info& target_type(){
            return typeid(meta_produce_t<to>);
        }
        consteval auto value(){
            return value_of<meta_produce_t<to>>;
        }
        consteval std::size_t left(){
            return exp_size<from>;
        }
        constexpr to_t object() const{
            static_assert(requires{to_t{};}, "can not construct object!");
            return to_t{};
        }
        consteval u128 flags(){
            return Control::value;
        }
    
    };

    // G_PACK / G_FEED_GEN 决定"喂"给用户回调的是什么，同时也决定循环结束时的 ::type。
    // 两边都用这一个别名算，保证运行期回调与编译期结果同步，不各写一遍。
    //   G_PACK 置位     → 打包视图 meta_pack_stream<Receiver, Generator, States, Driver>
    //   G_FEED_GEN 置位 → 生成器的产出（cache）
    //   都不置位        → Receiver 的产出
    // States 是携带这些控制位的 flags_operator 类型。
    template <class States, class Receiver, class Generator, class Driver = void>
    struct meta_states_feed_impl {
        static constexpr bool packed = States::template test<G_PACK>;
        static constexpr bool from_gen = States::template test<G_FEED_GEN>;
        static consteval auto result_type() {
            if constexpr (packed) {
                return std::type_identity<
                    meta_pack_stream<Receiver, Generator, States, Driver>>{};
            } else if constexpr (from_gen) {
                return std::type_identity<meta_produce_t<Generator>>{};
            } else {
                return std::type_identity<get_type<Receiver>>{};
            }
        }
        using type = typename decltype(result_type())::type;
    };

    template <class States, class Receiver, class Generator, class Driver = void>
    using meta_states_feed =
        typename meta_states_feed_impl<States, Receiver, Generator,
                                       Driver>::type;

    // 本轮记账（index 递增）已经交给 looper 自己的 control（user_flags_control）在
    // 一轮开头做，所以这里只负责消费掉 G_FINISH 这个一次性标记，
    // 保证下一轮开始时它是干净的（否则本轮被提前结束会污染下一轮的窗口推进）。
    template <class Flags>
    struct meta_states_settle_impl {
        using type = typename Flags::template opDeact<G_FINISH>;
    };

    template <class Flags>
    using meta_states_settle = typename meta_states_settle_impl<Flags>::type;

    // 一次位运算同时表达"SetMask 全部置位、ClearMask 全部清零"：
    //   (flags & (SetMask | ClearMask)) == SetMask
    // 这样 G_FINISH 这种"负位"（置位表示抑制）也能和正向的门位放在同一个比较里，
    // 不必拆成 a && b && !c。ClearMask 默认 0 时退化成纯 has<SetMask>。
    template <u64 SetMask, u64 ClearMask = 0>
    constexpr bool gated_bits(u128 flags) noexcept {
        return (flags & (SetMask | ClearMask)) == SetMask;
    }

    // 只采纳用户可写位；调度器拥有的位从调用前的快照恢复。
    constexpr u128 restore_owned_value(u128 mask, u128 before,
                                       u128 after) noexcept {
        return (after & ~mask) | (before & mask);
    }

    template<u128 Mask, class Before, class After>
    using restore_owned_flags = flags_operator<
        restore_owned_value(Mask, Before::value, After::value)>;

    // 决定标记 → 以当前 flags 为底的 flags。
    // SkipMask: skip 时清掉的位；YieldMask: yield 时置上的位。
    // 非标记的返回值原样保留（老的 flags 写法继续有效）。
    template<class Raw, class Flags, u128 SkipMask, u128 YieldMask>
    struct normalize_decision_impl {
        static consteval auto result_type() {
            if constexpr (std::is_same_v<Raw, meta_states_control::skip_t> ||
                          std::is_same_v<Raw, meta_states_control::yield_t> ||
                          std::is_same_v<Raw, meta_states_control::stop_t>) {
                constexpr u128 clear =
                    std::is_same_v<Raw, meta_states_control::skip_t> ? SkipMask :
                    std::is_same_v<Raw, meta_states_control::stop_t> ?
                        u128{G_CONTINUE} : u128{0};
                constexpr u128 set =
                    std::is_same_v<Raw, meta_states_control::yield_t> ?
                        YieldMask : u128{0};
                return std::type_identity<flags_operator<
                    (Flags::value & ~clear) | set>>{};
            } else {
                return std::type_identity<Raw>{};
            }
        }
        using type = typename decltype(result_type())::type;
    };
    template<class Raw, class Flags, u128 SkipMask, u128 YieldMask>
    using normalize_decision = typename normalize_decision_impl<
        Raw, Flags, SkipMask, YieldMask>::type;

    template<class Raw>
    constexpr u128 decision_value(u128 flags, u128 skip_mask,
                                   u128 yield_mask) noexcept {
        if constexpr (std::is_same_v<Raw, meta_states_control::skip_t>) {
            return flags & ~skip_mask;
        } else if constexpr (std::is_same_v<Raw, meta_states_control::yield_t>) {
            return flags | yield_mask;
        } else if constexpr (std::is_same_v<Raw, meta_states_control::stop_t>) {
            return flags & ~u128{G_CONTINUE};
        } else {
            return Raw::value;
        }
    }

    namespace meta_states_condition_funtion_detail{
        //control users' states flags
        //control users' states flags on stage index
        //and timer ticks and index inc logic
        //so that the user's flags get rid of the pushing on logic
        // 一轮 = 四个监控窗口，围着"生成"和"接收"两次执行
        //   ① pre_gen_exec   拿 Generator 调条件：要不要继续、要不要拉
        //   ② post_gen_exec  拿推进后的 Generator 调条件：拉到的东西怎么算
        //   ③ pre_rec_exec   拿 Receiver 调条件：这一步收不收、回调看什么
        //   ④ post_rec_exec  拿接收后的 Receiver 调条件：记账、下一轮怎么走
        constexpr u8 pre_gen_exec   = u8(0b00010000);
        constexpr u8 post_gen_exec  = u8(0b00100000);
        constexpr u8 pre_rec_exec   = u8(0b01000000);
        constexpr u8 post_rec_exec  = u8(0b10000000);
        constexpr u8 stage_index_mask  = u8(0b11110000);
        // 阶段字段在 looper 自己 flags 里的偏移（与上面的掩码对应：高位那一 nibble）
        constexpr std::size_t stage_shift = 4;

        // 用户 flags 里镜像出来的 stage_index：用 user 字段（u8）存，
        // 值就是四个窗口常量本身，用户的 condition 直接 get<u8, user_stage_shift> 分派。
        // 它由 looper 的 control 维护，用户函数不要再自己 push。
        constexpr std::size_t user_stage_shift = user_shift;
        inline constexpr u128 stage_mask =
            u128{stage_index_mask} << user_stage_shift;
        inline constexpr u128 scheduler_owned_mask =
            u128{position_mask} | stage_mask;

        // 窗口推进顺序：pre_gen → post_gen → pre_rec → post_rec →（回到本轮开头）
        constexpr u8 next_stage(u8 stage) noexcept {
            return stage == pre_gen_exec  ? post_gen_exec
                 : stage == post_gen_exec ? pre_rec_exec
                 : stage == pre_rec_exec  ? post_rec_exec
                 : pre_gen_exec;
        }

        // 把用户 flags 里的 stage_index（user 字段的高 4 位）写成 Stage。
        // user 字段的低 4 位是用户自己的，写回时原样保留。
        constexpr u128 with_stage_value(u128 flags, u8 stage) noexcept {
            return (flags & ~(u128{0xFF} << user_stage_shift)) |
                (u128{((flags.low >> user_stage_shift) & 0x0F) | stage}
                 << user_stage_shift);
        }

        template<u128 Flags, u8 Stage>
        using with_stage = flags_operator<with_stage_value(Flags, Stage)>;

        constexpr u128 settle_window_value(u128 flags, u8 stage) noexcept {
            const bool finished = (flags.low & G_FINISH) != 0 ||
                                  stage == post_rec_exec;
            return with_stage_value(
                finished ? meta_states_looper_bits::details::count_round(flags) : flags,
                finished ? pre_gen_exec : next_stage(stage));
        }

        template<u8 Stage, class Cond_o, class Obj>
        struct invoke_condition_impl {
            using staged_flags = with_stage<Cond_o::flags::value, Stage>;
            using staged = typename Cond_o::template meta_set<staged_flags>;
            using called = meta_object_invoke<staged, Obj>;
            using type = typename called::template meta_set<
                restore_owned_flags<scheduler_owned_mask, staged_flags,
                                    typename called::flags>>;
        };

        template<u8 Stage, class Cond_o, class Obj>
        using invoke_condition =
            typename invoke_condition_impl<Stage, Cond_o, Obj>::type;

        template<u8 Stage, class Cond_o, class Obj>
        struct invoke_window_impl {
            static constexpr u128 staged_value =
                with_stage_value(Cond_o::flags::value, Stage);
            using staged = typename Cond_o::template meta_set<flags_operator<staged_value>>;
            using called = meta_object_invoke<staged, Obj>;
            static constexpr u128 value = settle_window_value(
                restore_owned_value(scheduler_owned_mask, staged_value,
                                    called::flags::value), Stage);
            using type = typename called::template meta_set<flags_operator<value>>;
        };

        template<u8 Stage, class Cond_o, class Obj>
        using invoke_window = typename invoke_window_impl<Stage, Cond_o, Obj>::type;

        // looper 自己的 flags 对象：拿到"当前处于哪个窗口"（this_flags 里的阶段号）
        // 和完整的用户 condition 对象，显式读取 ::flags 决定写回用户的 flags。
        // 用户的 flags 从此不再需要自己 push 阶段号、也不再需要自己记账：
        //   中间的窗口①②③      : 只推进阶段号，不动 index
        //   一轮的结尾（窗口④）: index +1（记一步）、计时器递减、阶段号复位
        //   任意窗口若 G_FINISH 置位: 同样在本窗口记一步 + 递减 + 阶段号立刻复位
        //     （本轮就算提前结束，这一步也已经发生了，要记进去；阶段号复位让下一轮不错位）
        //
        // 记账放在轮尾（而不是轮首）的理由：
        //   轮首记账会让窗口①看到"这一步的编号"，而窗口②③④看到的却已经是加过一的新值 ——
        //   同一轮里四个窗口看到的 position 不一样，用户的函数只要按 position 判断就会错位。
        //   放在轮尾，同一轮四个窗口看到的 position 完全相同，它只表示"已经完整走完了多少轮"
        //   （= 这一步的编号）；窗口④的用户函数也还是看到旧值，回调和条件因此口径一致。
        //
        // 递减同样放在轮尾（而不是轮首）的理由：一轮里所有窗口看到的计数都是稳定的，
        // 计数 = 1 就正好是"从装表那一刻压到本轮结束"（即一整轮）；
        // 若放在轮首，装表那个窗口自己打上的计数会被同一轮其他窗口看到 0，
        // 于是装表点越靠前能压到的窗口越少。
        //
        // 注意：这里**不**动用户的门位。压制是 looper 判定时的事（见单步的 *_judge 值）：
        // 计时器只让 looper "看不见"那个位，而用户的 flags 原样保留。
        // 否则像 G_NOT_SKIP / G_PACK / G_FORWARD 这种只在种子/某一步里写一次、
        // 不会每轮重申的位，被压过就永久回不来了。
        struct user_flags_control{
            template<class this_flags, class Condition>
            struct apply{
                using user_flags = typename Condition::flags;
                // looper 自己 flags 里的窗口号（enter_window 现场设的）
                static constexpr u8 window =
                    this_flags::template get<u8, stage_shift>;
                // 用户 flags 里镜像的阶段号（用户函数就是按它分派的）
                static constexpr u8 user_stage =
                    user_flags::template get<u8, user_stage_shift>;
                static constexpr bool finished =
                    user_flags::template test<G_FINISH>;

                static constexpr auto value =
                    settle_window_value(user_flags::value, window);
            };
        };

        using user_flags_control_o = meta_states_object<0ull, user_flags_control>;

        // 默认窗口中转直接计算 flags；显式自定义 control 保留 MSO 调用协议。
        //   (1) 把 looper 自己 flags 里的阶段号设成 Stage（control 靠它知道自己在哪个窗口）
        //   (2) 用 control 去"调用"用户的 condition —— MSO 观察者收到完整对象，
        //       所以 control 的 apply 收到 (looper_flags, condition)，按需读取 condition::flags
        //   (3) 把 control 产出的 flags 写回用户的 condition，后面的窗口接着用
        // 注意：现在每个窗口的 control 都从新的一份 user_flags_control_o 起（只现场设阶段号）。
        // 如果以后 looper 自己的 flags 要在窗口之间累计，就把 control 对象像 condition 一样穿线。
        template<u8 Stage, class Cond_o, class Control_o = user_flags_control_o>
        struct enter_window_impl{
            static consteval auto result_type() {
                if constexpr (std::is_same_v<Control_o, user_flags_control_o>) {
                    return std::type_identity<typename Cond_o::template meta_set<
                        flags_operator<settle_window_value(Cond_o::flags::value, Stage)>>>{};
                } else {
                    using staged_control_o = typename Control_o::template meta_set<
                        typename Control_o::flags::template set<u8, stage_shift, Stage>>;
                    using control_result_o = meta_object_invoke<staged_control_o, Cond_o>;
                    return std::type_identity<
                        meta_transfer_object<control_result_o, Cond_o>>{};
                }
            }
            using type = typename decltype(result_type())::type;
        };

        template<u8 Stage, class Cond_o, class Control_o = user_flags_control_o>
        using enter_window =
            typename enter_window_impl<Stage, Cond_o, Control_o>::type;

        // 完整的一步窗口：先用 Obj 调一次用户的 condition（受 Gate 控制），再过上面的中转。
        // Gate 为假 = 整段窗口都不发生（条件不调、control 也不跑），对象原样传给下一个窗口
        // —— G_FINISH 就是这样逐个掐掉本轮后面的窗口的。
        template<bool Gate, u8 Stage, class Cond_o, class Obj>
        struct enter_window_step_impl{
            static consteval auto result_type() {
                if constexpr (Gate) {
                    return std::type_identity<invoke_window<Stage, Cond_o, Obj>>{};
                } else {
                    return std::type_identity<Cond_o>{};
                }
            }
            using type = typename decltype(result_type())::type;
        };
        template<bool Gate, u8 Stage, class Cond_o, class Obj>
        using enter_window_step =
            typename enter_window_step_impl<Gate, Stage, Cond_o, Obj>::type;
    }

    using meta_states_condition_funtion_detail::pre_gen_exec;
    using meta_states_condition_funtion_detail::post_gen_exec;
    using meta_states_condition_funtion_detail::pre_rec_exec;
    using meta_states_condition_funtion_detail::post_rec_exec;
    using meta_states_condition_funtion_detail::enter_window;
    using meta_states_condition_funtion_detail::enter_window_step;
    using meta_states_condition_funtion_detail::invoke_condition;
    using meta_states_condition_funtion_detail::invoke_window;
    using meta_states_condition_funtion_detail::user_flags_control_o;
    // ========================================================================
    // 单步：一轮的四窗口逻辑（从原来的 apply 里抽出来，不再递归）
    // ========================================================================
    template<class Condition, class Receiver, class Generator,
             class Driver = void, class... Args>
    struct meta_states_looper_step {
        // ---- 窗口① pre_gen_exec ----
        using pre_gen_exec_stage_o = invoke_window<pre_gen_exec, Condition, Generator>;
        static constexpr u128 pre_gen_judge =
            meta_states_looper_bits::details::effective_value(pre_gen_exec_stage_o::flags::value);
        static constexpr bool _entered_ =
            gated_bits<G_CONTINUE>(pre_gen_judge);
        static constexpr bool _advance_ =
            gated_bits<G_CONTINUE | G_ADVANCE, G_FINISH>(pre_gen_judge);
        using generator_stage_o =
            meta_invoke<invoke_if<_advance_>, Generator, Args...>;

        // ---- 窗口② post_gen_exec ----
        static constexpr bool _pre_gen_finished_ =
            gated_bits<G_FINISH>(pre_gen_judge);
        using post_gen_exec_stage_o = enter_window_step<
            !_pre_gen_finished_, post_gen_exec, pre_gen_exec_stage_o,
            generator_stage_o>;
        static constexpr u128 post_gen_judge =
            meta_states_looper_bits::details::effective_value(post_gen_exec_stage_o::flags::value);

        // ---- 窗口③ pre_rec_exec ----
        static constexpr bool _post_gen_finished_ =
            gated_bits<G_FINISH>(post_gen_judge);
        using pre_rec_exec_stage_o = enter_window_step<
            !_post_gen_finished_, pre_rec_exec, post_gen_exec_stage_o, Receiver>;
        static constexpr u128 pre_rec_judge =
            meta_states_looper_bits::details::effective_value(pre_rec_exec_stage_o::flags::value);

        static constexpr bool _receive_ =
            _entered_ &&
            gated_bits<G_CONTINUE | G_RECEIVE>(pre_rec_judge) &&
            gated_bits<0, G_FINISH>(post_gen_judge);

        static constexpr bool _forward_ =
            gated_bits<G_FORWARD>(pre_rec_judge);
        static constexpr bool _pre_rec_observe_ =
            gated_bits<G_CONTINUE | G_NOT_SKIP>(pre_rec_judge);

        using result_stage_o = meta_invoke<invoke_object_if<_receive_>,
                                           Receiver, generator_stage_o>;

        // ---- 窗口④ post_rec_exec ----
        static constexpr bool _pre_rec_finished_ =
            gated_bits<G_FINISH>(pre_rec_judge);
        using post_rec_exec_stage_o = enter_window_step<
            !_pre_rec_finished_, post_rec_exec, pre_rec_exec_stage_o,
            result_stage_o>;
        static constexpr u128 post_rec_judge =
            meta_states_looper_bits::details::effective_value(post_rec_exec_stage_o::flags::value);

        static constexpr bool _continue_ =
            _entered_ && gated_bits<G_CONTINUE>(post_rec_judge);
        static constexpr bool _observe_ = _pre_rec_observe_ && _continue_;

        using final_condition_flags_t =
            meta_states_settle<typename post_rec_exec_stage_o::flags>;
        using final_condition_o =
            typename post_rec_exec_stage_o::template meta_set<
                final_condition_flags_t>;

        using stage_t = meta_states_feed<typename final_condition_o::flags,
                                         result_stage_o, generator_stage_o,
                                         Driver>;

        static constexpr bool _swap_ = !_forward_ && _continue_;
        using next_receiver_o =
            std::conditional_t<_swap_, generator_stage_o, result_stage_o>;
        using next_generator_o =
            std::conditional_t<_swap_, result_stage_o, generator_stage_o>;

        // 对外暴露的单步结果（driver / chunk 只依赖这些）。
        using condition = final_condition_o;
        using receiver = next_receiver_o;
        using generator = next_generator_o;
        using stage = stage_t;
        static constexpr bool continue_ = _continue_;
        static constexpr bool observe_ = _observe_;
    };

    // ========================================================================
    // 分块续跑：状态 / 一段的结果
    // ========================================================================
    template<class Condition, class Receiver, class Generator>
    struct meta_states_looper_state {};

    // 惰性“未来有没有回调”：Current 为真时不去实例化 Rest::observes。
    // 直接写 Current || Rest::observes 时，编译器为了拿到这个成员仍然会
    // 实例化 Rest，即使 || 在常量求值时会短路。
    template<bool Current, class Rest>
    struct any_observes : std::bool_constant<[] {
        if constexpr (Current) return true;
        else return Rest::observes;
    }()> {};

    // 同理：从当前起的 N 轮里有没有回调。
    template<bool Current, std::size_t N, class Rest>
    struct any_within : std::bool_constant<[] {
        if constexpr (Current) return true;
        else return Rest::template observes_within_fn<N>();
    }()> {};

    // 一段最多跑 budget 轮。chunk 的 Remaining 从 R 递减到 0（共 R+1 轮），
    // driver 从 budget-1 起一段，所以满额正好是 budget 轮。
    inline constexpr std::size_t meta_states_looper_budget =
        META_STATES_LOOPER_BUDGET;
    static_assert(meta_states_looper_budget >= 1,
                  "META_STATES_LOOPER_BUDGET must be at least 1");

    // for_each 通过回调返回类型区分“观察值”和“控制决定”。
    template<class T>
    struct is_for_each_skip
        : std::bool_constant<std::is_same_v<T, meta_states_control::skip_t>> {};

    template<class T>
    struct is_for_each_yield
        : std::bool_constant<std::is_same_v<T, meta_states_control::yield_t>> {};

    template<class T>
    struct is_for_each_stop
        : std::bool_constant<std::is_same_v<T, meta_states_control::stop_t>> {};

    template<class T>
    inline constexpr bool is_for_each_decision_v =
        is_for_each_skip<std::remove_cvref_t<T>>::value ||
        is_for_each_yield<std::remove_cvref_t<T>>::value ||
        is_for_each_stop<std::remove_cvref_t<T>>::value;

    template<class T>
    inline constexpr bool is_for_each_stop_v =
        is_for_each_stop<std::remove_cvref_t<T>>::value;

    // ========================================================================
    // flags 驱动的 driver / chunk（新控制面）
    //
    // driver 是一个 MSO：它的 128 位 flags 就是 driver 的寄存器。
    // 低 64：数据/计数器；高 64：控制/状态。布局由 driver_control 自己定义。
    // ========================================================================
    namespace driver_control {
        // 低 64：数据 / 计数器
        inline constexpr std::size_t pc_shift = 0;            // u16
        inline constexpr std::size_t remaining_shift = 16;    // u8
        inline constexpr std::size_t chunk_index_shift = 24;  // u16
        inline constexpr std::size_t chunk_round_shift = 40;  // u8
        inline constexpr std::size_t timer0_shift = 48;       // u8
        inline constexpr std::size_t timer1_shift = 56;       // u8

        // 高 64：控制 / 状态
        inline constexpr std::size_t phase_shift = 64;   // u8
        inline constexpr std::size_t policy_shift = 80;  // u16
        inline constexpr std::size_t user_shift = 96;    // u32

        inline constexpr u128 observes_bit = u128{1} << 72;
        inline constexpr u128 has_more_bit = u128{1} << 73;
        inline constexpr u128 boundary_bit = u128{1} << 74;
        inline constexpr u128 stopped_bit = u128{1} << 75;
        inline constexpr u128 paused_bit = u128{1} << 76;
        inline constexpr u128 yielded_bit = u128{1} << 77;
        inline constexpr u128 skip_bit = u128{1} << 78;
        inline constexpr u128 error_bit = u128{1} << 79;

        inline constexpr u8 phase_run = 0;
        inline constexpr u8 phase_more = 1;
        inline constexpr u8 phase_done = 2;

        inline constexpr u8 window_round = 0;
        inline constexpr u8 window_more = 1;
        inline constexpr u8 window_decide = 3;

        template<class Step, u8 Window, class Decision = void>
        struct input {
            using step = Step;
            using decision = Decision;
            static constexpr u8 window = Window;
        };

        constexpr u128 round_value(u128 flags, bool observe,
                                    bool cont) noexcept {
            static_assert(pc_shift == 0);
            const u8 remaining = static_cast<u8>(flags.low >> remaining_shift);
            const u8 timer0 = static_cast<u8>(flags.low >> timer0_shift);
            const u8 timer1 = static_cast<u8>(flags.low >> timer1_shift);
            constexpr u64 counters_mask =
                (u64{0xFFFF} << pc_shift) |
                (u64{0xFF} << remaining_shift) |
                (u64{0xFF} << chunk_round_shift) |
                (u64{0xFF} << timer0_shift) |
                (u64{0xFF} << timer1_shift);
            flags.low = (flags.low & ~counters_mask) |
                (u64{static_cast<u16>(flags.low + 1)} << pc_shift) |
                ((u64{remaining} - (remaining != 0)) << remaining_shift) |
                (u64{static_cast<u8>((flags.low >> chunk_round_shift) + 1)}
                 << chunk_round_shift) |
                ((u64{timer0} - (timer0 != 0)) << timer0_shift) |
                ((u64{timer1} - (timer1 != 0)) << timer1_shift);
            const u8 done = static_cast<u8>(!cont);
            const u8 more = static_cast<u8>(cont && (remaining == 0));
            const u8 phase = static_cast<u8>(done * phase_done +
                                             more * phase_more);
            flags.high = (flags.high & ~u64{0xFF}) | phase |
                (u64{observe} * observes_bit.high) |
                (u64{phase == phase_more} *
                 (has_more_bit.high | boundary_bit.high)) |
                (u64{!cont} * stopped_bit.high);
            return flags;
        }

        constexpr u128 more_value(u128 flags, u8 seed_remaining) noexcept {
            constexpr u64 reset_mask =
                (u64{0xFF} << remaining_shift) |
                (u64{0xFFFF} << chunk_index_shift) |
                (u64{0xFF} << chunk_round_shift);
            flags.low = (flags.low & ~reset_mask) |
                (u64{seed_remaining} << remaining_shift) |
                (u64{static_cast<u16>((flags.low >> chunk_index_shift) + 1)}
                 << chunk_index_shift);
            flags.high &= ~(u64{0xFF} | has_more_bit.high | boundary_bit.high);
            return flags;
        }

        constexpr u128 decide_value(u128 flags, bool skip, bool yield,
                                    bool stop) noexcept {
            const u64 phase = flags.high & u64{0xFF};
            const u64 next_phase = phase +
                static_cast<u64>(stop) * (phase_done - phase);
            flags.high = (flags.high & ~u64{0xFF}) | next_phase |
                (u64{skip} * skip_bit.high) |
                (u64{yield} * yielded_bit.high) |
                (u64{stop} * stopped_bit.high);
            return flags;
        }
    }  // namespace driver_control

    template<std::size_t Budget>
    struct default_driver_policy {
        static_assert(Budget >= 1, "driver budget must be at least 1");
        static_assert(Budget <= 256,
                      "driver remaining is u8; budget must be <= 256");
        static constexpr u8 seed_remaining =
            static_cast<u8>(Budget - 1);

        template<class Flags, class Input>
        struct round {
            static constexpr bool observe = Input::step::observe_;
            static constexpr bool cont = Input::step::continue_;

            static constexpr u128 value =
                driver_control::round_value(Flags::value, observe, cont);
            using type = flags_operator<value>;
        };

        template<class Flags, class Input>
        struct more {
            static constexpr u128 value =
                driver_control::more_value(Flags::value, seed_remaining);
            using type = flags_operator<value>;
        };

        template<class Flags, class Input>
        struct decide {
            using decision = typename Input::decision;
            static constexpr bool skip =
                std::is_same_v<decision, meta_states_control::skip_t>;
            static constexpr bool yield =
                std::is_same_v<decision, meta_states_control::yield_t>;
            static constexpr bool stop =
                std::is_same_v<decision, meta_states_control::stop_t>;
            static constexpr u128 value = driver_control::decide_value(
                Flags::value, skip, yield, stop);
            using type = flags_operator<value>;
        };
    };

    template<class Policy>
    struct driver_fn {
        template<class Flags, class Input>
        struct apply {
            static constexpr u128 value = [] {
                if constexpr (Input::window == driver_control::window_round) {
                    return Policy::template round<Flags, Input>::value;
                } else if constexpr (Input::window ==
                                     driver_control::window_more) {
                    return Policy::template more<Flags, Input>::value;
                } else {
                    return Policy::template decide<Flags, Input>::value;
                }
            }();
        };
    };

    template<std::size_t Budget,
             class Policy = default_driver_policy<Budget>>
    using flags_driver_seed_flags =
        flags_operator<u128{static_cast<u8>(Policy::seed_remaining)}
                       << driver_control::remaining_shift>;

    template<std::size_t Budget, class Policy = default_driver_policy<Budget>>
    using flags_driver_mso = meta_states_object<
        flags_driver_seed_flags<Budget, Policy>::value, driver_fn<Policy>>;

    template<class Driver, class State, class... Args>
    struct flags_chunk;
    template<class Driver, class State, class... Args>
    struct flags_driver;

    template<class Driver, class Condition, class Receiver, class Generator,
             class... Args>
    struct flags_chunk<
        Driver, meta_states_looper_state<Condition, Receiver, Generator>,
        Args...> {
        using step =
            meta_states_looper_step<Condition, Receiver, Generator, Driver,
                                    Args...>;
        using round_driver = meta_invoke<Driver,
            driver_control::input<step, driver_control::window_round>>;

        static constexpr u8 phase = round_driver::flags::template get<
            u8, driver_control::phase_shift>;
        static constexpr std::size_t rounds =
            Driver::flags::template get<u8,
                                        driver_control::remaining_shift> +
            1;

        using step_next_state =
            meta_states_looper_state<typename step::condition,
                                     typename step::receiver,
                                     typename step::generator>;
        using step_next_driver = meta_invoke<round_driver,
            driver_control::input<step, driver_control::window_more>>;
        using rest = flags_chunk<round_driver, step_next_state, Args...>;

        static consteval bool has_more_fn() {
            if constexpr (phase == driver_control::phase_more) {
                return true;
            } else if constexpr (phase == driver_control::phase_run) {
                return rest::has_more;
            } else {
                return false;
            }
        }
        static constexpr bool has_more = has_more_fn();

        static consteval auto next_driver_fn() {
            if constexpr (phase == driver_control::phase_run) {
                return std::type_identity<
                    typename rest::next_driver>{};
            } else {
                return std::type_identity<step_next_driver>{};
            }
        }
        using next_driver = typename decltype(next_driver_fn())::type;

        static consteval auto next_state_fn() {
            if constexpr (phase == driver_control::phase_run) {
                return std::type_identity<
                    typename rest::next_state>{};
            } else {
                return std::type_identity<step_next_state>{};
            }
        }
        using next_state = typename decltype(next_state_fn())::type;

        static consteval auto feed_fn() {
            if constexpr (phase == driver_control::phase_done) {
                return std::type_identity<typename step::stage>{};
            } else if constexpr (phase == driver_control::phase_more) {
                return std::type_identity<void>{};
            } else {
                return std::type_identity<typename rest::feed>{};
            }
        }
        using feed = typename decltype(feed_fn())::type;

        static constexpr bool observes = [] {
            if constexpr (phase == driver_control::phase_done ||
                          phase == driver_control::phase_more) {
                return step::observe_;
            } else {
                return any_observes<step::observe_, rest>::value;
            }
        }();

        template<std::size_t N>
        static consteval bool observes_within_fn() {
            if constexpr (phase != driver_control::phase_run) {
                return N > 0 && step::observe_;
            } else if constexpr (N == 0) {
                return false;
            } else {
                return any_within<step::observe_, N - 1, rest>::value;
            }
        }

        template<std::size_t N>
        static consteval bool future_within_args() {
            if constexpr (phase != driver_control::phase_run) {
                return false;
            } else if constexpr (N == 0) {
                return false;
            } else if constexpr (rest::template observes_within_fn<N>()) {
                return true;
            } else if constexpr (rest::has_more && N > rest::rounds) {
                return flags_driver<
                    typename rest::next_driver,
                    typename rest::next_state,
                    Args...>::template observes_within_fn<
                    N - rest::rounds>();
            } else {
                return false;
            }
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (phase != driver_control::phase_run) {
                if constexpr (step::observe_) {
                    return std::invoke(f, typename step::stage{},
                                       std::forward<A>(a)...);
                } else {
                    return;
                }
            } else {
                if constexpr (step::observe_) {
                    using return_type = std::invoke_result_t<
                        F&, typename step::stage, A...>;
                    if constexpr (is_for_each_decision_v<return_type>) {
                        if constexpr (is_for_each_stop_v<return_type>) {
                            return std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                        } else if constexpr (rest::observes) {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                            return rest::for_each(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        } else {
                            return std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                        }
                    } else if constexpr (rest::observes) {
                        if constexpr (std::is_void_v<return_type>) {
                            std::invoke(f, typename step::stage{},
                                        std::forward<A>(a)...);
                        } else {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<A>(a)...);
                        }
                        return rest::for_each(std::forward<F>(f),
                                              std::forward<A>(a)...);
                    } else {
                        return std::invoke(f, typename step::stage{},
                                           std::forward<A>(a)...);
                    }
                } else {
                    if constexpr (rest::observes) {
                        return rest::for_each(std::forward<F>(f),
                                              std::forward<A>(a)...);
                    } else {
                        return;
                    }
                }
            }
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                         A&&... a) {
            if constexpr (phase == driver_control::phase_done) {
                if constexpr (step::observe_) {
                    return std::invoke(f, typename step::stage{},
                                       std::forward<First>(first));
                } else {
                    return;
                }
            } else if constexpr (phase == driver_control::phase_more) {
                using driver =
                    flags_driver<next_driver, next_state, Args...>;
                if constexpr (step::observe_) {
                    using return_type = std::invoke_result_t<
                        F&, typename step::stage, First>;
                    if constexpr (is_for_each_stop_v<return_type>) {
                        return std::invoke(
                            f, typename step::stage{},
                            std::forward<First>(first));
                    } else if constexpr (is_for_each_decision_v<
                                             return_type>) {
                        if constexpr (driver::template observes_within_fn<
                                          sizeof...(A)>()) {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<First>(first));
                            return driver::for_each_forward(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        } else {
                            return std::invoke(
                                f, typename step::stage{},
                                std::forward<First>(first));
                        }
                    } else if constexpr (driver::template observes_within_fn<
                                             sizeof...(A)>()) {
                        if constexpr (std::is_void_v<return_type>) {
                            std::invoke(f, typename step::stage{},
                                        std::forward<First>(first));
                            return driver::for_each_forward(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        } else {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<First>(first));
                            return driver::for_each_forward(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        }
                    } else {
                        return std::invoke(f, typename step::stage{},
                                           std::forward<First>(first));
                    }
                } else {
                    if constexpr (driver::template observes_within_fn<
                                      sizeof...(A)>()) {
                        return driver::for_each_forward(
                            std::forward<F>(f), std::forward<A>(a)...);
                    } else {
                        return;
                    }
                }
            } else {
                if constexpr (step::observe_) {
                    using return_type = std::invoke_result_t<
                        F&, typename step::stage, First>;
                    if constexpr (is_for_each_stop_v<return_type>) {
                        return std::invoke(
                            f, typename step::stage{},
                            std::forward<First>(first));
                    } else if constexpr (is_for_each_decision_v<
                                             return_type>) {
                        if constexpr (future_within_args<
                                          sizeof...(A)>()) {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<First>(first));
                            return rest::for_each_forward(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        } else {
                            return std::invoke(
                                f, typename step::stage{},
                                std::forward<First>(first));
                        }
                    } else if constexpr (future_within_args<
                                             sizeof...(A)>()) {
                        if constexpr (std::is_void_v<return_type>) {
                            std::invoke(f, typename step::stage{},
                                        std::forward<First>(first));
                        } else {
                            (void)std::invoke(
                                f, typename step::stage{},
                                std::forward<First>(first));
                        }
                        return rest::for_each_forward(
                            std::forward<F>(f), std::forward<A>(a)...);
                    } else {
                        return std::invoke(f, typename step::stage{},
                                           std::forward<First>(first));
                    }
                } else {
                    if constexpr (future_within_args<sizeof...(A)>()) {
                        return rest::for_each_forward(
                            std::forward<F>(f), std::forward<A>(a)...);
                    } else {
                        return;
                    }
                }
            }
        }
    };

    template<class Driver, class State, class... Args>
    struct flags_driver {
        using chunk = flags_chunk<Driver, State, Args...>;

        template<class Decision>
        using decide = meta_invoke<Driver,
            driver_control::input<typename chunk::step,
                                  driver_control::window_decide,
                                  Decision>>;

        static consteval auto result_type() {
            if constexpr (chunk::has_more) {
                return std::type_identity<typename flags_driver<
                    typename chunk::next_driver,
                    typename chunk::next_state,
                    Args...>::type>{};
            } else {
                return std::type_identity<typename chunk::feed>{};
            }
        }
        using type = typename decltype(result_type())::type;

        struct observes_t {
            static constexpr bool value = [] {
                if constexpr (!chunk::has_more) {
                    return chunk::observes;
                } else {
                    using next = flags_driver<typename chunk::next_driver,
                                              typename chunk::next_state,
                                              Args...>;
                    return any_observes<chunk::observes, next>::value;
                }
            }();
        };
        static constexpr bool observes = observes_t::value;

        template<std::size_t N>
        static consteval bool observes_within_fn() {
            if constexpr (N == 0) {
                return false;
            } else if constexpr (chunk::template observes_within_fn<N>()) {
                return true;
            } else if constexpr (chunk::has_more &&
                                 N > chunk::rounds) {
                return flags_driver<
                    typename chunk::next_driver,
                    typename chunk::next_state,
                    Args...>::template observes_within_fn<
                    N - chunk::rounds>();
            } else {
                return false;
            }
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (chunk::has_more) {
                using chunk_return = decltype(chunk::for_each(
                    std::forward<F>(f), std::forward<A>(a)...));
                if constexpr (is_for_each_stop_v<chunk_return>) {
                    return chunk::for_each(std::forward<F>(f),
                                           std::forward<A>(a)...);
                } else {
                    using next = flags_driver<typename chunk::next_driver,
                                              typename chunk::next_state,
                                              Args...>;
                    if constexpr (next::observes_t::value) {
                        if constexpr (chunk::observes) {
                            (void)chunk::for_each(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        }
                        return next::for_each(std::forward<F>(f),
                                              std::forward<A>(a)...);
                    } else {
                        if constexpr (chunk::observes) {
                            return chunk::for_each(
                                std::forward<F>(f),
                                std::forward<A>(a)...);
                        } else {
                            return;
                        }
                    }
                }
            } else {
                if constexpr (chunk::observes) {
                    return chunk::for_each(std::forward<F>(f),
                                           std::forward<A>(a)...);
                } else {
                    return;
                }
            }
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                         A&&... a) {
            return chunk::for_each_forward(std::forward<F>(f),
                                           std::forward<First>(first),
                                           std::forward<A>(a)...);
        }
    };


    // ========================================================================
    // 公开入口：形状保持不变
    // ========================================================================
    template<meta_states_object_t Condition, meta_object_t Receiver,
             meta_object_t Generator = meta_empty_o,
             class DriverPolicy =
                 default_driver_policy<meta_states_looper_budget>>
    struct meta_states_looper {
        using main_condition = Condition;

        template<class... Args>
        struct apply {
            using driver = flags_driver<
                flags_driver_mso<meta_states_looper_budget, DriverPolicy>,
                meta_states_looper_state<Condition, Receiver, Generator>,
                Args...>;
            using type = typename driver::type;

            template<class Decision>
            using decide = typename driver::template decide<Decision>;

            template<class F, class... A>
            static constexpr decltype(auto) for_each(F&& f, A&&... a) {
                return driver::for_each(std::forward<F>(f),
                                        std::forward<A>(a)...);
            }

            template<class F, class First, class... A>
            static constexpr decltype(auto) for_each_forward(
                F&& f, First&& first, A&&... a) {
                return driver::for_each_forward(
                    std::forward<F>(f), std::forward<First>(first),
                    std::forward<A>(a)...);
            }
        };

        template<class Decision, class... Args>
        using decide = typename meta_invoke<meta_states_looper, Args...>
            ::template decide<Decision>;
    };

    template <class Condition, class Receiver, class Generator, class... ARG_Tys>
    using meta_states_looper_t = typename meta_invoke<
        meta_states_looper<Condition, Receiver, Generator>,
        ARG_Tys...>::type;

    template <class Condition, class Receiver, class Generator,
              class DriverPolicy, class... ARG_Tys>
    using meta_states_looper_with_policy_t = typename meta_invoke<
        meta_states_looper<Condition, Receiver, Generator, DriverPolicy>,
        ARG_Tys...>::type;

    // meta_looper 的 meta_for 对应物：条件必须是 meta_states_object。
    // Generator 默认 meta_empty_o（“没有生成器”的写法）。
    template <meta_states_object_t Con, meta_object_t To,
              meta_object_t From = meta_empty_o, class... Args>
    using meta_states_for =
        meta_invoke<meta_states_looper<Con, To, From>, Args...>;

    template <meta_states_object_t Con, meta_object_t To,
              meta_object_t From, class DriverPolicy, class... Args>
    using meta_states_for_with_policy =
        meta_invoke<meta_states_looper<Con, To, From, DriverPolicy>, Args...>;
}
