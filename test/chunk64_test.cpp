#include "meta_chunk64.hpp"
#include "meta_objects.hpp"

using namespace meta_chunk64;
using namespace meta_objects;
using exp_utilities::exp_list;

// ---- test-only slicing helpers ----
template <u64 N, class TL>
struct drop_n;
template <class... Ts>
struct drop_n<0, exp_list<Ts...>> { using type = exp_list<Ts...>; };
template <u64 N, class... Ts>
struct drop_n<N, exp_list<Ts...>> {
  static_assert(N > 0);
  using type = typename drop_n<N - 1, typename exp_list<Ts...>::pop_front>::type;
};

// segment size 2; segment N = elements [2N, 2N+2)
template <class... Ts>
struct chunk_list : chunked_able {
  template <u64 States>
  using chunk = exp_utilities::take<
      2, typename drop_n<States * 2, exp_list<Ts...>>::type>;
};

// payload without chunk
using plain = exp_list<int, double>;

struct fn { template <class...> using apply = exp_list<>; };

consteval bool chunks_the_payload() {
  using obj = meta_object<chunk_list<int, double, char, long>, fn>;

  using seg0 = chunk_object<obj, 0>;
  static_assert(std::is_same_v<typename seg0::type, exp_list<int, double>>);

  using seg1 = chunk_object<obj, 1>;
  static_assert(std::is_same_v<typename seg1::type, exp_list<char, long>>);

  // 函数与既有载荷之外的东西保持不变
  static_assert(std::is_same_v<typename seg1::function, fn>);
  return true;
}
static_assert(chunks_the_payload());

consteval bool plain_payload_untouched() {
  using obj = meta_object<plain, fn>;
  static_assert(std::is_same_v<chunk_object<obj, 0>, obj>);
  static_assert(std::is_same_v<chunk_object<obj, 7>, obj>);
  return true;
}
static_assert(plain_payload_untouched());

consteval bool concept_checks() {
  static_assert(chunked_able_c<chunk_list<int>>);
  static_assert(!chunked_able_c<plain>);
  static_assert(next_segment(0) == 1 && next_segment(41) == 42);
  return true;
}
static_assert(concept_checks());

int main() { return 0; }
