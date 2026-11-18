#pragma once

// ============================================================================
// meta_chunk64: payload-level chunking at driver boundaries.
//
// The payload decides how to cut itself; the driver only asks for segment N at
// a boundary and puts the result back with meta_set. Nothing here interprets
// what the payload does - `States` is opaque to the driver.
//
// Payload protocol:
//   struct my_list : meta_chunk64::chunked_able {
//     template <u64 States> using chunk = /* the payload for that segment */;
//   };
//
// Driver rule:
//   - only a payload with a `chunk` member template is chunked;
//   - `chunk<States>` is called at a boundary, never mid-round;
//   - the result replaces the payload through `Obj::meta_set<...>`.
// ============================================================================

#include <cstdint>

namespace meta_chunk64 {

using u64 = std::uint64_t;

// Optional marker base; the concept is the real check.
struct chunked_able {};

template <class T>
concept chunked_able_c = requires { typename T::template chunk<u64{0}>; };

// The driver only ever increments the segment counter; the payload decides what
// a segment is (size, slice, remainder...). Bit 0..31 = index, 32..63 reserved.
constexpr u64 next_segment(u64 states) noexcept { return states + 1; }

namespace details {
template <class Obj, u64 States, bool Chunked>
struct chunk_object_impl {
  using type = Obj;   // payload has no chunk: leave the object alone
};

template <class Obj, u64 States>
struct chunk_object_impl<Obj, States, true> {
  using type = typename Obj::template meta_set<
      typename Obj::type::template chunk<States>>;
};
}  // namespace details

// Chunk one boundary: returns the object with its payload replaced by
// `Payload::chunk<States>`, or the object unchanged when the payload is not
// chunked_able.
template <class Obj, u64 States>
using chunk_object = typename details::chunk_object_impl<
    Obj, States, chunked_able_c<typename Obj::type>>::type;

}  // namespace meta_chunk64
