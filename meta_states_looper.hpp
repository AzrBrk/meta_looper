#pragma once

#include "meta_objects.hpp"

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
// 要强行换表用 override_timer。
// Count 必须 >= 1 —— 计时器只有"正在计时"一种有效状态，
// 写 count = 0 只会留下一个没意义的残字段（要清表用 clear_timer）。
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using set_timer = std::conditional_t<
    (timer_count<Flags, Slot> != 0), Flags,
    typename Flags::template set<u16, details::timer_shift<Slot>,
        static_cast<u16>((Count << timer_count_shift) | std::countr_zero(Target))>>;

// 强行覆盖同一个槽上正在走的计时器（无视它还在计时）。
// 想换目标位、改计数、或者把已经在走的表推倒重来，用这个。
template<flags_operator_type Flags, std::size_t Slot, u64 Target,
         std::size_t Count>
  requires (Slot < 2 && Count > 0 && Count <= timer_max_count &&
            looper_controlled_bit<Target>)
using override_timer = typename Flags::template set<u16, details::timer_shift<Slot>,
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

    template<
        meta_object_t Receiver, 
        meta_object_t Generator,
        flags_operator_type Control>
    struct meta_pack_stream{
        using from   = Generator;
        using to     = Receiver;
        using to_t   = typename to::type;
        using from_t = typename from::type;
        using cache  = meta_produce_t<Generator>;
        using states = Control;

        consteval std::size_t index(){
            // position 在轮尾（窗口④ / G_FINISH）才 +1，而这份视图是轮尾之后建好的，
            // 所以减 1 才是"本轮的编号"；这正好等于本轮四个窗口里用户函数看到的 position。
            return states::template get<u16, position_shift> - 1;
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
                               typename Receiver::type>>;
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
    template<bool continue_v, 
        meta_states_object_t Condition,
        meta_object_t Receiver, 
        meta_object_t Generator
        >
    struct meta_states_looper_impl{
        
        using main_condition = Condition;
        template<class ...Args>
        struct apply{
            // ===================== 控制点 START =====================
            // 一轮 = 四个监控窗口，围着"生成"和"接收"两次执行：
            //   ① pre_gen_exec   拿 Generator 调条件 —— 要不要继续(G_CONTINUE)、要不要拉(G_ADVANCE)
            //   ② post_gen_exec  拿推进后的 Generator 调条件 —— 拉到的东西怎么算
            //   ③ pre_rec_exec   拿 Receiver 调条件 —— 这一步收不收(G_RECEIVE)、回调看什么
            //   ④ post_rec_exec  拿接收后的 Receiver 调条件 —— 记账(G_FINISH)、下一轮怎么走
            // 每个窗口结束后都过一遍 looper 自己的 control（user_flags_control_o），
            // 由它决定写回用户 condition 的 flags（现在还是恒等传递）。
            // 用户 condition 的状态是累计的：后一个窗口用前一个窗口的产物。
            //
            // ---- 窗口① pre_gen_exec ----
            // 产出 flags 字，控制位在低 64 位的最高 8 位（gates 字节）：
            // G_CONTINUE 置位 = 循环继续；G_ADVANCE 置位 = 生成器推进。
            using gen_states_o = meta_object_invoke<Condition, Generator>;
            using pre_gen_exec_stage_o = enter_window<pre_gen_exec, gen_states_o>;

            // 判定一律透过计时器看：计时器压的是"looper 的判定"，不是用户的 flags。
            // 这样 G_NOT_SKIP / G_PACK / G_FORWARD 这类不每轮重申的位被压之后能自己恢复。
            using pre_gen_judge = effective<typename pre_gen_exec_stage_o::flags>;

            static constexpr bool _continue_ =
                pre_gen_judge::template test<G_CONTINUE>;
            static constexpr bool _advance_ =
                gated_bits<G_CONTINUE | G_ADVANCE, G_FINISH>(pre_gen_judge::value);

            using generator_stage_o =
                meta_invoke<invoke_if<_advance_>, Generator, Args...>;

            // ---- 窗口② post_gen_exec ----
            // ① 打了 G_FINISH（"本轮到此为止"）时，这个窗口整段不发生。
            static constexpr bool _pre_gen_finished_ =
                pre_gen_judge::template test<G_FINISH>;
            using post_gen_exec_stage_o =
                enter_window_step<!_pre_gen_finished_, post_gen_exec,
                                  pre_gen_exec_stage_o, generator_stage_o>;
            using post_gen_judge = effective<typename post_gen_exec_stage_o::flags>;

           


            // ---- 窗口③ pre_rec_exec ----
            // ② 打了 G_FINISH 时整段不发生。
            static constexpr bool _post_gen_finished_ =
                post_gen_judge::template test<G_FINISH>;
            using pre_rec_exec_stage_o =
                enter_window_step<!_post_gen_finished_, pre_rec_exec,
                                  post_gen_exec_stage_o, Receiver>;
            using pre_rec_judge = effective<typename pre_rec_exec_stage_o::flags>;

            // 用新状态的 G_CONTINUE && G_RECEIVE 判断这一步 Receiver 是否接收。
            // 参与位（RECEIVE）看窗口③的产出；抑制位（FINISH）看窗口② —— ② 自己打的
            // FINISH 意思是"本轮到此为止"，掐掉的正是它后面的这次接收；③ 自己打的 FINISH
            // 只表示"本轮记账"，不掐掉它自己驱动的这次接收。
            // 再与 _continue_ 相与：这一步如果整体不发生（_continue_ 假），
            // Receiver 也不该被应用，否则结束时的 ::type 会比最后一次回调多走一步。
            static constexpr bool _receive_ =
                _continue_ &&
                gated_bits<G_CONTINUE | G_RECEIVE>(pre_rec_judge::value) &&
                gated_bits<0, G_FINISH>(post_gen_judge::value);

            // G_FORWARD 控制传输方向：置位 = 保持 Gen->Rec；清掉 = 下一阶段把
            // Receiver 和 Generator 的位置互换（变成 Rec->Gen）。
            // 只换位置：互换之后这两个元对象彼此怎么调用，是元对象调用协议的事情，
            // looper 不参与，也不替它们做适配。
            static constexpr bool _forward_ =
                pre_rec_judge::template test<G_FORWARD>;

            // G_FEED_GEN / G_PACK 控制回调喂哪一段：G_PACK 优先（置位时忽略
            // G_FEED_GEN）。这两个标志同时决定循环结束时的 ::type，见 meta_states_feed。
            static constexpr bool _pack_ =
                pre_rec_judge::template test<G_PACK>;
            static constexpr bool _feed_gen_ =
                pre_rec_judge::template test<G_FEED_GEN>;

            // 回调门：G_NOT_SKIP 决定"这一步要不要回调用户"，G_FINISH 不参与
            //（它只管本轮的元对象调用与记账）。G_CONTINUE 是总闸 —— 它决定整个循环
            // 是否继续，所以也参与本门：总闸关掉时这一步根本不存在，回调自然不发生，
            // 两个静态函数直接返回、什么也不做。
            // _observe_ 为假时，下面是编译期把整段回调代码消灭掉的分支。
            static constexpr bool _observe_ =
                gated_bits<G_CONTINUE | G_NOT_SKIP>(pre_rec_judge::value);

            // Apply the generated input to the current state when receiving;
            // otherwise retain the current state as the final result.
            using result_stage_o =
                meta_invoke<invoke_object_if<_receive_>, Receiver, generator_stage_o>;

            // ---- 窗口④ post_rec_exec ----
            // ③ 打了 G_FINISH（"接收照做，但本轮到此为止"）时整段不发生。
            static constexpr bool _pre_rec_finished_ =
                pre_rec_judge::template test<G_FINISH>;
            using post_rec_exec_stage_o =
                enter_window_step<!_pre_rec_finished_, post_rec_exec,
                                  pre_rec_exec_stage_o, result_stage_o>;

            // G_FINISH 由 condition 的 function 标记，意思是"本轮到此为止"：
            //   - looper 侧：掐掉本轮后面的窗口（逐个窗口往下判）；
            //   - control 侧：不记账、不递减计时器，并把阶段号复位（见 user_flags_control）。
            // 它不影响下一轮是否继续（后者是 G_CONTINUE = while/for 的 continue 语义）。
            // 一次性标记，结转时消费掉。
            using final_condition_flags_t =
                meta_states_settle<typename post_rec_exec_stage_o::flags>;
            using final_condition_o =
                typename post_rec_exec_stage_o::template meta_set<final_condition_flags_t>;

            // 用户回调看到的东西；false 分支的 ::type 用同一个别名（同一份 flags，
            // 即已结转的 final_condition_o::flags），所以两者不会各算一套。
            using stage_t = meta_states_feed<typename final_condition_o::flags,
                                             result_stage_o, generator_stage_o>;
            // ===================== 控制点 END =======================

            // 注意：meta_object_t 的 payload 不一定是类型列表（参见
            // meta_ios::index_sequence_istream，payload 是 index_counter）。
            // "还剩多少"用 exp_size<...>（payload 有 length 即可），
            // "是否到头"用 is_end_stream<...>（payload 自己继承 end_of_stream<...>）。
            // 不要对 payload 用 list_end / exp_list / pop_front / front 这一套。

            // Instantiate the next iteration only when the condition remains true.
            // _forward_ 为真：位置不变（Gen->Rec）；为假：下一阶段互换位置（Rec->Gen）。
            // 只有真的还有下一阶段（_continue_）时才互换，否则结束时的 ::type 会用错角色，
            // 和这一步回调看到的类型对不上。
            static constexpr bool _swap_ = !_forward_ && _continue_;
            using next_receiver_o =
                std::conditional_t<_swap_, generator_stage_o, result_stage_o>;
            using next_generator_o =
                std::conditional_t<_swap_, result_stage_o, generator_stage_o>;

            // 条件状态累计：把这一轮结转后的状态 final_condition_o 传给下一轮。
            using track_apply_t =
                meta_invoke<invoke_if<_continue_>,
                            meta_states_looper_impl<_continue_, final_condition_o,
                                                    next_receiver_o, next_generator_o>,
                            Args...>;
            using type = typename track_apply_t::type;

            // ===================== 返回值归属 START =====================
            // 用户回调的返回值只保留"最后一个真的被回调过的步"的那一个。
            // 被跳过的步（_observe_ 假）根本没调用 f，它没有值可返回；而递归的
            // 收尾恰恰常常落在这样一个跳过步上（例如总闸在窗口②被关掉，同一步的
            // 窗口③就不再回调），如果老老实实递归到底，终点那个跳过步就会吐 void，
            // 把前面那个真值整个丢掉 —— 于是 for_each 的返回值算不出来。
            // 所以：只要后面已经没有会回调的步，本步就立刻返回自己的值，不再往下走
            // （往下的步对回调而言本来就是空转）。
            static constexpr bool _next_observes_ =
                track_apply_t::_continue_ && track_apply_t::_observes_deeper_;
            // 本步或更深的任意一步会不会回调用户
            static constexpr bool _observes_deeper_ = _observe_ || _next_observes_;
            // ===================== 返回值归属 END =======================
            template <class... arg_types>
            static constexpr decltype(auto) for_each(auto&& f, arg_types&&... args) {
              if constexpr (!_observe_) {
                // 本轮不回调：整段回调代码（含 f 的调用、返回类型推导）在编译期被消灭
                if constexpr (_next_observes_) {
                  return track_apply_t::for_each(f,
                                                 std::forward<arg_types>(args)...);
                } else {
                  return;  // 后面没有会被回调的步：整条链本来就没有值
                }
              } else {
                // 只有最终执行到的那一步的返回值决定整条链的返回类型
                //（前面各步都是 (void) 丢弃后转发递归的结果），所以返回类型唯一。
                using return_type =
                    std::invoke_result_t<decltype(f), stage_t, arg_types...>;

                if constexpr (std::is_void_v<return_type>) {
                  if constexpr (_next_observes_) {
                    std::invoke(f, stage_t{}, std::forward<arg_types>(args)...);
                    return track_apply_t::for_each(f,
                                                   std::forward<arg_types>(args)...);
                  } else {
                    return std::invoke(f, stage_t{},
                                       std::forward<arg_types>(args)...);
                  }
                } else {
                  if constexpr (_next_observes_) {
                    (void)std::invoke(f, stage_t{},
                                      std::forward<arg_types>(args)...);
                    return track_apply_t::for_each(f,
                                                   std::forward<arg_types>(args)...);
                  } else {
                    return std::invoke(f, stage_t{},
                                       std::forward<arg_types>(args)...);
                  }
                }
              }
            }

            template <class first_arg_type, class... arg_types>
            static constexpr decltype(auto) for_each_forward(auto&& f,
                                                             first_arg_type&& first,
                                                             arg_types&&... args) {
              if constexpr (!_observe_) {
                // 本轮跳过：这一步的参数直接丢弃，f 的调用完全不实例化，
                // 参数按位置继续往下一步传。
                if constexpr (_next_observes_ && sizeof...(arg_types)) {
                  return track_apply_t::for_each_forward(
                      f, std::forward<arg_types>(args)...);
                } else {
                  return;  // 后面没有会被回调的步：整条链本来就没有值
                }
              } else {
                // 命中步的返回值就是整条链的返回类型（前面各步 (void) 丢弃后转发），
                // 所以 f 可以在不同类型的位置参数上转发，最终只漏出被命中的那一个。
                using return_type =
                    std::invoke_result_t<decltype(f), stage_t, first_arg_type>;

                if constexpr (std::is_void_v<return_type>) {
                  if constexpr (_next_observes_ &&
                                sizeof...(arg_types)) {
                    std::invoke(f, stage_t{},
                                std::forward<first_arg_type>(first));
                    return track_apply_t::for_each_forward(
                        f, std::forward<arg_types>(args)...);
                  } else {
                    return std::invoke(f, stage_t{},
                                       std::forward<first_arg_type>(first));
                  }
                } else {
                  if constexpr (_next_observes_ &&
                                sizeof...(arg_types)) {
                    (void)std::invoke(f, stage_t{},
                                      std::forward<first_arg_type>(first));
                    return track_apply_t::for_each_forward(
                        f, std::forward<arg_types>(args)...);
                  } else {
                    return std::invoke(f, stage_t{},
                                       std::forward<first_arg_type>(first));
                  }
                }
              }
            }
        };
    };

    // 条件为假：循环结束。产出必须和用户回调看到的是同一种东西，
    // 所以这里用同一个 meta_states_feed 算 ::type（G_PACK / G_FEED_GEN 生效）。
    template<class Condition, class Receiver, class Generator>
    struct meta_states_looper_impl<false, Condition, Receiver, Generator>{
        static constexpr bool _continue_ = false;
        // 走到这里 = 循环已经结束，后面不可能再有会回调用户的步。
        static constexpr bool _observes_deeper_ = false;
        using type = meta_states_feed<typename Condition::flags, Receiver, Generator>;
        template<class ...Args>
        struct apply{
            using type = meta_states_feed<typename Condition::flags, Receiver, Generator>;
            template<class... arg_types>
            static constexpr decltype(auto) for_each(auto&&, arg_types&&...) { return; }
            template<class first_arg_type, class... arg_types>
            static constexpr decltype(auto) for_each_forward(auto&&, first_arg_type&&, arg_types&&...) { return; }
        };
    };

    template <class Condition, class Receiver, class Generator, class... ARG_Tys>
    using meta_states_looper_t = typename meta_invoke<
        meta_states_looper_impl<true, Condition, Receiver, Generator>,
        ARG_Tys...>::type;

    template <class Condition, class Receiver, class Generator = meta_empty_o>
    using meta_states_looper =
        meta_states_looper_impl<true, Condition, Receiver, Generator>;

    // meta_looper 的 meta_for 对应物：条件必须是 meta_states_object。
    // Generator 默认 meta_empty_o（“没有生成器”的写法）。
    template <meta_states_object_t Con, meta_object_t To,
              meta_object_t From = meta_empty_o, class... Args>
    using meta_states_for =
        meta_invoke<meta_states_looper<Con, To, From>, Args...>;
}