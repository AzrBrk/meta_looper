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
using effective = typename Flags::template opDeact<
    details::suppression(timer_value<Flags, 0>) |
    details::suppression(timer_value<Flags, 1>)>;

// 装表：同一个槽位**还在计时**（计数 != 0）时不覆盖，原样返回 Flags。
// 要强行换表用 override_suppression_timer。
// Count 必须 >= 1 —— 计时器只有"正在计时"一种有效状态，
// 写 count = 0 只会留下一个没意义的残字段（要清表用 clear_timer）。
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using set_suppression_timer = std::conditional_t<
    (timer_count<Flags, Slot> != 0), Flags,
    typename Flags::template set<u16, details::timer_shift<Slot>,
        static_cast<u16>((Count << timer_count_shift) | std::countr_zero(Target))>>;

// 强行覆盖同一个槽上正在走的计时器（无视它还在计时）。
// 想换目标位、改计数、或者把已经在走的表推倒重来，用这个。
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using override_suppression_timer = typename Flags::template set<u16, details::timer_shift<Slot>,
    static_cast<u16>((Count << timer_count_shift) | std::countr_zero(Target))>;

template<flags_operator_type Flags, std::size_t Slot> requires (Slot < 2)
using clear_timer = typename Flags::template deact<u16, details::timer_shift<Slot>>;

template<flags_operator_type Flags>
using tick_timers = typename Flags
    ::template set<u16, timer0_shift, details::tick(timer_value<Flags, 0>)>
    ::template set<u16, timer1_shift, details::tick(timer_value<Flags, 1>)>;
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
        flags_operator_type Control>
    struct meta_pack_stream{
        using from   = Generator;
        using to     = Receiver;
        using to_t   = get_type<to>;
        using from_t = get_type<from>;
        using cache  = meta_produce_t<Generator>;
        using states = Control;

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
    
    };

    // G_PACK / G_FEED_GEN 决定"喂"给用户回调的是什么，同时也决定循环结束时的 ::type。
    // 两边都用这一个别名算，保证运行期回调与编译期结果同步，不各写一遍。
    //   G_PACK 置位     → 打包视图 meta_pack_stream<Receiver, Generator, States>
    //   G_FEED_GEN 置位 → 生成器的产出（cache）
    //   都不置位        → Receiver 的产出
    // States 是携带这些控制位的 flags_operator 类型。
    template <class States, class Receiver, class Generator>
    struct meta_states_feed_impl {
        static constexpr bool packed = States::template test<G_PACK>;
        static constexpr bool from_gen = States::template test<G_FEED_GEN>;
        using type = std::conditional_t<
            packed, meta_pack_stream<Receiver, Generator, States>,
            std::conditional_t<from_gen, meta_produce_t<Generator>,
                               get_type<Receiver>>>;
    };

    template <class States, class Receiver, class Generator>
    using meta_states_feed =
        typename meta_states_feed_impl<States, Receiver, Generator>::type;

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

    // 决定标记 → 以当前 flags 为底的 flags。
    // SkipMask: skip 时清掉的位；YieldMask: yield 时置上的位。
    // 非标记的返回值原样保留（老的 flags 写法继续有效）。
    template<class Raw, class Flags, u128 SkipMask, u128 YieldMask>
    struct normalize_decision_impl {
        using type = Raw;
    };
    template<class Flags, u128 SkipMask, u128 YieldMask>
    struct normalize_decision_impl<meta_states_control::skip_t, Flags,
                                   SkipMask, YieldMask> {
        using type = typename Flags::template opDeact<SkipMask>;
    };
    template<class Flags, u128 SkipMask, u128 YieldMask>
    struct normalize_decision_impl<meta_states_control::yield_t, Flags,
                                   SkipMask, YieldMask> {
        using type = typename Flags::template opSet<YieldMask>;
    };
    template<class Flags, u128 SkipMask, u128 YieldMask>
    struct normalize_decision_impl<meta_states_control::stop_t, Flags,
                                   SkipMask, YieldMask> {
        using type = typename Flags::template opDeact<G_CONTINUE>;
    };
    template<class Raw, class Flags, u128 SkipMask, u128 YieldMask>
    using normalize_decision = typename normalize_decision_impl<
        Raw, Flags, SkipMask, YieldMask>::type;

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

        // 窗口推进顺序：pre_gen → post_gen → pre_rec → post_rec →（回到本轮开头）
        constexpr u8 next_stage(u8 stage) noexcept {
            return stage == pre_gen_exec  ? post_gen_exec
                 : stage == post_gen_exec ? pre_rec_exec
                 : stage == pre_rec_exec  ? post_rec_exec
                 : pre_gen_exec;
        }

        // 把用户 flags 里的 stage_index（user 字段的高 4 位）写成 Stage。
        // user 字段的低 4 位是用户自己的，写回时原样保留。
        template<u128 Flags, u8 Stage>
        using with_stage = typename flags_operator<Flags>::template set<u8, user_stage_shift,
            static_cast<u8>(((Flags >> user_stage_shift) & u8(0x0F)) | Stage)>;

        // looper 自己的 flags 对象：拿到"当前处于哪个窗口"（this_flags 里的阶段号）
        // 和"用户 condition 现在的 flags"（user_flags），决定写回用户的 flags。
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
        // 注意：这里**不**动用户的门位。压制是 looper 判定时的事（见 apply 里的 *_judge 别名）：
        // 计时器只让 looper "看不见"那个位，而用户的 flags 原样保留。
        // 否则像 G_NOT_SKIP / G_PACK / G_FORWARD 这种只在种子/某一步里写一次、
        // 不会每轮重申的位，被压过就永久回不来了。
        struct user_flags_control{
            template<class this_flags, class user_flags>
            struct apply{
                // looper 自己 flags 里的窗口号（enter_window 现场设的）
                static constexpr u8 window =
                    this_flags::template get<u8, stage_shift>;
                // 用户 flags 里镜像的阶段号（用户函数就是按它分派的）
                static constexpr u8 user_stage =
                    user_flags::template get<u8, user_stage_shift>;
                static constexpr bool finished =
                    user_flags::template test<G_FINISH>;

                // 记一步：index（position 字段）+1
                using counted = typename user_flags::template inc<u16, position_shift>;
                // 记一步之后再递减计时器：details::tick 自带 0 计数防护（计数为 0 时
                // 原样返回，且减到 0 时把整个字段清掉 → timer_value == 0 真正等于"空槽"）
                using round_done = tick_timers<counted>;

                static constexpr auto value = []{
                    if constexpr (finished || window == post_rec_exec) {
                        // 本轮结束（正常走到窗口④，或在任意窗口打了 G_FINISH）：
                        // 现在才记账 + 递减 + 阶段号复位。
                        // 递减必须挂"本轮结束"这个事件上，而不是死挂在窗口④ ——
                        // 否则在窗口④打 G_FINISH 的用法会把窗口④掐掉，计数就永远不走了。
                        return with_stage<round_done::value, pre_gen_exec>::value;
                    } else {
                        // 中间的窗口：只推进阶段号，不记账 —— 同一轮里四个窗口
                        // 看到的 position 因此完全一致。
                        return with_stage<user_flags::value, next_stage(window)>::value;
                    }
                }();
            };
        };

        using user_flags_control_o = meta_states_object<0ull, user_flags_control>;

        // 一个监控窗口的中转：让 looper 自己的 control 看一次用户的 condition。
        //   (1) 把 looper 自己 flags 里的阶段号设成 Stage（control 靠它知道自己在哪个窗口）
        //   (2) 用 control 去"调用"用户的 condition —— MSO 互相调用会传对方的 flags，
        //       所以 control 的 apply 收到 (looper_flags, user_flags)
        //   (3) 把 control 产出的 flags 写回用户的 condition，后面的窗口接着用
        // 注意：现在每个窗口的 control 都从新的一份 user_flags_control_o 起（只现场设阶段号）。
        // 如果以后 looper 自己的 flags 要在窗口之间累计，就把 control 对象像 condition 一样穿线。
        template<u8 Stage, class Cond_o, class Control_o = user_flags_control_o>
        struct enter_window_impl{
            using staged_control_o = typename Control_o::template meta_set<
                typename Control_o::flags::template set<u8, stage_shift, Stage>>;
            using control_result_o = meta_object_invoke<staged_control_o, Cond_o>;
            using type = meta_transfer_object<control_result_o, Cond_o>;
        };

        template<u8 Stage, class Cond_o, class Control_o = user_flags_control_o>
        using enter_window =
            typename enter_window_impl<Stage, Cond_o, Control_o>::type;

        // 完整的一步窗口：先用 Obj 调一次用户的 condition（受 Gate 控制），再过上面的中转。
        // Gate 为假 = 整段窗口都不发生（条件不调、control 也不跑），对象原样传给下一个窗口
        // —— G_FINISH 就是这样逐个掐掉本轮后面的窗口的。
        template<bool Gate, u8 Stage, class Cond_o, class Obj>
        struct enter_window_step_impl{
            using called_o = meta_invoke<invoke_object_if<Gate>, Cond_o, Obj>;
            using type = enter_window<Stage, called_o>;
        };
        template<u8 Stage, class Cond_o, class Obj>
        struct enter_window_step_impl<false, Stage, Cond_o, Obj>{
            using type = Cond_o;
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
    using meta_states_condition_funtion_detail::user_flags_control_o;
    // ========================================================================
    // 单步：一轮的四窗口逻辑（从原来的 apply 里抽出来，不再递归）
    // ========================================================================
    template<class Condition, class Receiver, class Generator, class... Args>
    struct meta_states_looper_step {
        // ---- 窗口① pre_gen_exec ----
        using gen_states_o = meta_object_invoke<Condition, Generator>;
        using pre_gen_exec_stage_o = enter_window<pre_gen_exec, gen_states_o>;
        using pre_gen_judge = effective<typename pre_gen_exec_stage_o::flags>;
        static constexpr bool _entered_ =
            pre_gen_judge::template test<G_CONTINUE>;
        static constexpr bool _advance_ =
            gated_bits<G_CONTINUE | G_ADVANCE, G_FINISH>(pre_gen_judge::value);
        using generator_stage_o =
            meta_invoke<invoke_if<_advance_>, Generator, Args...>;

        // ---- 窗口② post_gen_exec ----
        static constexpr bool _pre_gen_finished_ =
            pre_gen_judge::template test<G_FINISH>;
        using post_gen_exec_stage_o = enter_window_step<
            !_pre_gen_finished_, post_gen_exec, pre_gen_exec_stage_o,
            generator_stage_o>;
        using post_gen_judge = effective<typename post_gen_exec_stage_o::flags>;

        // ---- 窗口③ pre_rec_exec ----
        static constexpr bool _post_gen_finished_ =
            post_gen_judge::template test<G_FINISH>;
        using pre_rec_exec_stage_o = enter_window_step<
            !_post_gen_finished_, pre_rec_exec, post_gen_exec_stage_o, Receiver>;
        using pre_rec_judge = effective<typename pre_rec_exec_stage_o::flags>;

        static constexpr bool _receive_ =
            _entered_ &&
            gated_bits<G_CONTINUE | G_RECEIVE>(pre_rec_judge::value) &&
            gated_bits<0, G_FINISH>(post_gen_judge::value);

        static constexpr bool _forward_ =
            pre_rec_judge::template test<G_FORWARD>;
        static constexpr bool _pre_rec_observe_ =
            gated_bits<G_CONTINUE | G_NOT_SKIP>(pre_rec_judge::value);

        using result_stage_o = meta_invoke<invoke_object_if<_receive_>,
                                           Receiver, generator_stage_o>;

        // ---- 窗口④ post_rec_exec ----
        static constexpr bool _pre_rec_finished_ =
            pre_rec_judge::template test<G_FINISH>;
        using post_rec_exec_stage_o = enter_window_step<
            !_pre_rec_finished_, post_rec_exec, pre_rec_exec_stage_o,
            result_stage_o>;
        using post_rec_judge = effective<typename post_rec_exec_stage_o::flags>;

        static constexpr bool _continue_ =
            _entered_ && post_rec_judge::template test<G_CONTINUE>;
        static constexpr bool _observe_ = _pre_rec_observe_ && _continue_;

        using final_condition_flags_t =
            meta_states_settle<typename post_rec_exec_stage_o::flags>;
        using final_condition_o =
            typename post_rec_exec_stage_o::template meta_set<
                final_condition_flags_t>;

        using stage_t = meta_states_feed<typename final_condition_o::flags,
                                         result_stage_o, generator_stage_o>;

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

    template<class Feed>
    struct meta_states_looper_done {
        using feed = Feed;
    };

    template<class State>
    struct meta_states_looper_more {
        using state = State;
    };

    // 惰性“未来有没有回调”：Current 为真时不去实例化 Rest::observes。
    // 直接写 Current || Rest::observes 时，编译器为了拿到这个成员仍然会
    // 实例化 Rest，即使 || 在常量求值时会短路。
    template<bool Current, class Rest>
    struct any_observes : std::bool_constant<true> {};

    template<class Rest>
    struct any_observes<false, Rest>
        : std::bool_constant<Rest::observes> {};

    // 同理：从当前起的 N 轮里有没有回调。
    template<bool Current, std::size_t N, class Rest>
    struct any_within : std::bool_constant<true> {};

    template<std::size_t N, class Rest>
    struct any_within<false, N, Rest>
        : std::bool_constant<Rest::template observes_within_fn<N>()> {};

    // 一段最多跑 budget 轮。chunk 的 Remaining 从 R 递减到 0（共 R+1 轮），
    // driver 从 budget-1 起一段，所以满额正好是 budget 轮。
    inline constexpr std::size_t meta_states_looper_budget =
        META_STATES_LOOPER_BUDGET;
    static_assert(meta_states_looper_budget >= 1,
                  "META_STATES_LOOPER_BUDGET must be at least 1");

    template<class State, std::size_t Remaining, class... Args>
    struct meta_states_looper_chunk;
    template<class State, class... Args>
    struct meta_states_looper_driver;

    // 一段的尾部：正常结束 / 预算用完（more）/ 继续本段下一轮
    template<bool Continue, bool AtBudget, class Step, class NextState,
             std::size_t Remaining, class... Args>
    struct meta_states_looper_chunk_tail;

    // 正常结束：本段的最后一轮。
    template<bool AtBudget, class Step, class NextState, std::size_t Remaining,
             class... Args>
    struct meta_states_looper_chunk_tail<false, AtBudget, Step, NextState,
                                         Remaining, Args...> {
        using type = meta_states_looper_done<typename Step::stage>;
        static constexpr bool has_more = false;
        using next_state = NextState;
        static constexpr bool observes = Step::observe_;

        // 本段从当前轮起的 N 轮里有没有回调（只算本段）。
        template<std::size_t N>
        static consteval bool observes_within_fn() {
            return N > 0 && Step::observe_;
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (Step::observe_) {
                return std::invoke(f, typename Step::stage{},
                                   std::forward<A>(a)...);
            } else {
                return;
            }
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                         A&&...) {
            if constexpr (Step::observe_) {
                return std::invoke(f, typename Step::stage{},
                                   std::forward<First>(first));
            } else {
                return;
            }
        }
    };

    // 预算用完：把状态交给 driver 续跑。
    template<class Step, class NextState, std::size_t Remaining, class... Args>
    struct meta_states_looper_chunk_tail<true, true, Step, NextState,
                                         Remaining, Args...> {
        using driver = meta_states_looper_driver<NextState, Args...>;
        using type = meta_states_looper_more<NextState>;
        static constexpr bool has_more = true;
        using next_state = NextState;
        static constexpr bool observes = Step::observe_;

        template<std::size_t N>
        static consteval bool observes_within_fn() {
            return N > 0 && Step::observe_;
        }

        // 段边界：只处理本段最后一轮；continuation 由 driver 顺序调用。
        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (Step::observe_) {
                return std::invoke(f, typename Step::stage{},
                                   std::forward<A>(a)...);
            } else {
                return;
            }
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                         A&&... a) {
            if constexpr (Step::observe_) {
                using return_type =
                    std::invoke_result_t<F&, typename Step::stage, First>;
                if constexpr (driver::template observes_within_fn<sizeof...(A)>()) {
                    if constexpr (std::is_void_v<return_type>) {
                        std::invoke(f, typename Step::stage{},
                                    std::forward<First>(first));
                        return driver::for_each_forward(
                            std::forward<F>(f), std::forward<A>(a)...);
                    } else {
                        (void)std::invoke(f, typename Step::stage{},
                                          std::forward<First>(first));
                        return driver::for_each_forward(
                            std::forward<F>(f), std::forward<A>(a)...);
                    }
                } else {
                    return std::invoke(f, typename Step::stage{},
                                       std::forward<First>(first));
                }
            } else {
                if constexpr (driver::template observes_within_fn<sizeof...(A)>()) {
                    return driver::for_each_forward(
                        std::forward<F>(f), std::forward<A>(a)...);
                } else {
                    return;
                }
            }
        }
    };

    // 继续本段下一轮。
    template<class Step, class NextState, std::size_t Remaining, class... Args>
    struct meta_states_looper_chunk_tail<true, false, Step, NextState,
                                         Remaining, Args...> {
        using rest =
            meta_states_looper_chunk<NextState, Remaining - 1, Args...>;
        using type = typename rest::type;
        static constexpr bool has_more = rest::has_more;
        using next_state = typename rest::next_state;
        static constexpr bool observes =
            any_observes<Step::observe_, rest>::value;

        // 本步之后（只算本段剩余轮次）还有没有回调。
        // continuation 由 driver 负责，不能在这里探测：chunk 只跑自己这段，
        // 这样 for_each 的函数体实例化链在段边界处被打断。
        static consteval bool future_observes() {
            return rest::observes;
        }

        // 本段从当前轮起的 N 轮里有没有回调（不含 continuation）。
        template<std::size_t N>
        static consteval bool observes_within_fn() {
            if constexpr (N == 0) {
                return false;
            } else {
                return any_within<Step::observe_, N - 1, rest>::value;
            }
        }

        // 本段剩余 + continuation，前 N 个剩余轮次里有没有回调。
        // 跨段的 for_each_forward 用它判断“当前返回值之后会不会被覆盖”。
        template<std::size_t N>
        static consteval bool future_within_args() {
            if constexpr (N == 0) {
                return false;
            } else if constexpr (rest::template observes_within_fn<N>()) {
                return true;
            } else if constexpr (rest::has_more && N > rest::rounds) {
                return meta_states_looper_driver<
                    typename rest::next_state,
                    Args...>::template observes_within_fn<N - rest::rounds>();
            } else {
                return false;
            }
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (Step::observe_) {
                using return_type =
                    std::invoke_result_t<F&, typename Step::stage, A...>;
                if constexpr (future_observes()) {
                    if constexpr (std::is_void_v<return_type>) {
                        std::invoke(f, typename Step::stage{},
                                    std::forward<A>(a)...);
                    } else {
                        (void)std::invoke(f, typename Step::stage{},
                                          std::forward<A>(a)...);
                    }
                    return rest::for_each(std::forward<F>(f),
                                          std::forward<A>(a)...);
                } else {
                    return std::invoke(f, typename Step::stage{},
                                       std::forward<A>(a)...);
                }
            } else {
                if constexpr (future_observes()) {
                    return rest::for_each(std::forward<F>(f),
                                          std::forward<A>(a)...);
                } else {
                    return;
                }
            }
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                         A&&... a) {
            if constexpr (Step::observe_) {
                using return_type =
                    std::invoke_result_t<F&, typename Step::stage, First>;
                if constexpr (future_within_args<sizeof...(A)>()) {
                    if constexpr (std::is_void_v<return_type>) {
                        std::invoke(f, typename Step::stage{},
                                    std::forward<First>(first));
                    } else {
                        (void)std::invoke(f, typename Step::stage{},
                                          std::forward<First>(first));
                    }
                    return rest::for_each_forward(
                        std::forward<F>(f), std::forward<A>(a)...);
                } else {
                    return std::invoke(f, typename Step::stage{},
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
    };

    // ========================================================================
    // 一段：Remaining 递减到 0，共 Remaining+1 轮（driver 从 budget-1 起 → 满额 budget 轮）
    // ========================================================================
    template<class Condition, class Receiver, class Generator,
             std::size_t Remaining, class... Args>
    struct meta_states_looper_chunk<
        meta_states_looper_state<Condition, Receiver, Generator>,
        Remaining, Args...> {
        using step =
            meta_states_looper_step<Condition, Receiver, Generator, Args...>;
        using step_next_state =
            meta_states_looper_state<typename step::condition,
                                     typename step::receiver,
                                     typename step::generator>;
        using tail = meta_states_looper_chunk_tail<
            step::continue_, (Remaining == 0), step, step_next_state,
            Remaining, Args...>;

        using type = typename tail::type;
        static constexpr bool has_more = tail::has_more;
        using next_state = typename tail::next_state;
        static constexpr bool observes = tail::observes;
        // 本段满额时的轮数：Remaining 从 R 递减到 0，共 R+1 轮。
        static constexpr std::size_t rounds = Remaining + 1;

        template<std::size_t N>
        static consteval bool observes_within_fn() {
            return tail::template observes_within_fn<N>();
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            return tail::for_each(std::forward<F>(f), std::forward<A>(a)...);
        }

        template<class F, class First, class... A>
        static constexpr decltype(auto) for_each_forward(F&& f, First&& first,
                                                         A&&... a) {
            return tail::for_each_forward(std::forward<F>(f),
                                          std::forward<First>(first),
                                          std::forward<A>(a)...);
        }
    };

    // ========================================================================
    // driver：把 more 一段段续完
    // ========================================================================
    template<class R, class... Args>
    struct meta_states_looper_driver_type;

    template<class Feed, class... Args>
    struct meta_states_looper_driver_type<meta_states_looper_done<Feed>,
                                          Args...> {
        using type = Feed;
    };

    template<class State, class... Args>
    struct meta_states_looper_driver_type<meta_states_looper_more<State>,
                                          Args...> {
        using type = typename meta_states_looper_driver<State, Args...>::type;
    };

    template<class State, class... Args>
    struct meta_states_looper_driver_observes;

    template<bool HasMore, class State, class... Args>
    struct meta_states_looper_next_observes {
        static constexpr bool observes = false;
    };
    template<class State, class... Args>
    struct meta_states_looper_next_observes<true, State, Args...> {
        static constexpr bool observes =
            meta_states_looper_driver_observes<State, Args...>::value;
    };

    template<class State, class... Args>
    struct meta_states_looper_driver_observes {
        using chunk = meta_states_looper_chunk<
            State, meta_states_looper_budget - 1, Args...>;
        static constexpr bool value =
            any_observes<
                chunk::observes,
                meta_states_looper_next_observes<
                    chunk::has_more, typename chunk::next_state,
                    Args...>>::value;
    };

    template<class State, class... Args>
    struct meta_states_looper_driver {
        using chunk = meta_states_looper_chunk<
            State, meta_states_looper_budget - 1, Args...>;
        using type = typename meta_states_looper_driver_type<
            typename chunk::type, Args...>::type;

        struct observes_t {
            static constexpr bool value =
                meta_states_looper_driver_observes<State, Args...>::value;
        };

        // 从 driver 当前位置起的 N 轮里有没有回调（可跨段）。
        // for_each_forward 用它判断“当前返回值之后会不会被覆盖”。
        template<std::size_t N>
        static consteval bool observes_within_fn() {
            if constexpr (N == 0) {
                return false;
            } else if constexpr (!chunk::has_more) {
                return chunk::template observes_within_fn<N>();
            } else if constexpr (N <= chunk::rounds) {
                return chunk::template observes_within_fn<N>();
            } else {
                return any_within<
                    chunk::template observes_within_fn<N>(),
                    N - chunk::rounds,
                    meta_states_looper_driver<
                        typename chunk::next_state, Args...>>::value;
            }
        }

        template<class F, class... A>
        static constexpr decltype(auto) for_each(F&& f, A&&... a) {
            if constexpr (chunk::has_more) {
                using next_driver =
                    meta_states_looper_driver<typename chunk::next_state,
                                              Args...>;
                if constexpr (next_driver::observes_t::value) {
                    // 本段还有回调就先执行（副作用），结果会被后面的段覆盖。
                    if constexpr (chunk::observes) {
                        (void)chunk::for_each(std::forward<F>(f),
                                              std::forward<A>(a)...);
                    }
                    return next_driver::for_each(std::forward<F>(f),
                                                 std::forward<A>(a)...);
                } else {
                    if constexpr (chunk::observes) {
                        return chunk::for_each(std::forward<F>(f),
                                               std::forward<A>(a)...);
                    } else {
                        return;
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
             meta_object_t Generator = meta_empty_o>
    struct meta_states_looper {
        using main_condition = Condition;

        template<class... Args>
        struct apply {
            using driver = meta_states_looper_driver<
                meta_states_looper_state<Condition, Receiver, Generator>,
                Args...>;
            using type = typename driver::type;

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
    };

    template <class Condition, class Receiver, class Generator, class... ARG_Tys>
    using meta_states_looper_t = typename meta_invoke<
        meta_states_looper<Condition, Receiver, Generator>,
        ARG_Tys...>::type;

    // meta_looper 的 meta_for 对应物：条件必须是 meta_states_object。
    // Generator 默认 meta_empty_o（“没有生成器”的写法）。
    template <meta_states_object_t Con, meta_object_t To,
              meta_object_t From = meta_empty_o, class... Args>
    using meta_states_for =
        meta_invoke<meta_states_looper<Con, To, From>, Args...>;
}
