#include "meta_ios.hpp"

#ifndef META_BENCH_ROUNDS
#define META_BENCH_ROUNDS 65
#endif
#ifndef META_BENCH_NODES
#define META_BENCH_NODES 4
#endif
#ifndef META_BENCH_STRIDE
#define META_BENCH_STRIDE 2
#endif
#ifndef META_BENCH_INITIALIZED
#define META_BENCH_INITIALIZED 1
#endif

namespace compile_matrix {
using namespace meta_ios;
using namespace meta_iostream;
static_assert(META_BENCH_NODES >= 1 && META_BENCH_NODES <= 64);
static_assert(META_BENCH_STRIDE >= 1);

struct filter {
  template<class Window, class Flags, class Object>
  struct apply {
    static constexpr auto value = [] {
      if constexpr (Window::value == meta_states::post_gen_exec) {
        constexpr bool keep =
            meta_objects::meta_produce_t<Object>::value % META_BENCH_STRIDE == 0;
        constexpr auto mask = meta_states::u128{meta_states::G_RECEIVE |
                                               meta_states::G_NOT_SKIP} |
                              meta_stream_details::pipe_emit_bit;
        return keep ? Flags::value | mask : Flags::value & ~mask;
      } else {
        return Flags::value;
      }
    }();
  };
};

template<class Sequence>
struct list;
template<std::size_t... I>
struct list<std::index_sequence<I...>> {
  using type = exp_utilities::exp_list<std::integral_constant<std::size_t, I>...>;
};
using input = typename list<std::make_index_sequence<META_BENCH_ROUNDS>>::type;
using source = typename decltype([] {
  if constexpr (META_BENCH_INITIALIZED) {
    return std::type_identity<index_sequence_istream<META_BENCH_ROUNDS>>{};
  } else {
    return std::type_identity<meta_istream<input>>{};
  }
}())::type;

template<std::size_t Remaining, class Builder>
struct append {
  using type = typename append<Remaining - 1,
      typename Builder::template all_to<meta_iterator>>::type;
};
template<class Builder>
struct append<0, Builder> {
  using type = Builder;
};
template<std::size_t Nodes, class Source>
struct make_pipeline {
  using type = typename decltype([] {
    if constexpr (Nodes == 1) {
      return std::type_identity<typename meta_pipe<Source>::template run<
          meta_iterator, con<node_con<filter, true>>>>{};
    } else {
      using first = typename meta_pipe<Source>::template all_to<
          meta_iterator, con<node_con<filter, true>>>;
      using chain = typename append<Nodes - 2, first>::type;
      return std::type_identity<typename chain::template run<meta_iterator>>{};
    }
  }())::type;
};
using pipeline = typename make_pipeline<META_BENCH_NODES, source>::type;
static_assert(sizeof(typename pipeline::type) != 0);
constexpr auto callbacks = [] {
  std::size_t count = 0;
  pipeline::for_each([&](auto) { ++count; });
  return count;
}();
static_assert(callbacks ==
    (META_BENCH_ROUNDS + META_BENCH_STRIDE - 1) / META_BENCH_STRIDE);
}  // namespace compile_matrix

int main() {}
