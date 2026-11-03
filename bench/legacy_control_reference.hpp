#pragma once

#include "../meta_states.hpp"

namespace legacy_control_reference {
using namespace meta_states;
namespace windows = meta_states_looper_details::meta_states_condition_funtion_detail;

constexpr u128 with_stage(u128 flags, u8 stage) noexcept {
  const u64 byte = (flags.low >> windows::user_stage_shift) & 0xFF;
  return (flags & ~(u128{0xFF} << windows::user_stage_shift)) |
      (u128{(byte & 0x0F) | stage} << windows::user_stage_shift);
}

constexpr u128 settle(u128 flags, u8 stage, bool failed = false) noexcept {
  const bool receiver_failed = stage == post_rec_exec && failed;
  flags = (flags & ~(receiver_failed ? u128{G_CONTINUE} : u128{0})) |
      (receiver_failed ? receiver_init_failed_mask : u128{0});
  const bool finished = (flags.low & G_FINISH) || stage == post_rec_exec;
  return with_stage(finished ? bits::details::count_round(flags) : flags,
                    finished ? pre_gen_exec : windows::next_stage(stage));
}

struct control {
  template<class Flags, class Condition>
  struct apply {
    static constexpr auto value = [] {
      const u8 stage = [] {
        if constexpr (meta_objects::meta_op_bits::is_segment_bank_v<decltype(Flags::value)>)
          return static_cast<u8>(Flags::value.segments[0] >> windows::stage_shift);
        else return Flags::template get<u8, windows::stage_shift>;
      }();
      auto settled = settle(Condition::flags::value, stage);
      if constexpr (meta_objects::meta_op_bits::is_segment_bank_v<decltype(Flags::value)>) {
        auto bank = Flags::value;
        bank.import_u128(settled);
        return bank;
      } else return settled;
    }();
  };
};
}