#include "meta_objects.hpp"

using namespace meta_objects;

struct ready_initializer {
  template<class...>
  using initialize = int;
  template<class Current, class...>
  using apply = Current;
};

struct deferred_initializer {
  template<class Input> requires (!no_arguments<Input>)
  using initialize = Input;
  template<class Current, class...>
  using apply = Current;
};

struct identity {
  template<class Current, class...>
  using apply = Current;
};

struct argument_tag {};

template<class Expected>
struct inspect_original {
  template<class Flags, class Object, class Argument>
  struct apply {
    static_assert(std::is_same_v<Object, Expected>);
    static_assert(std::is_same_v<Argument, argument_tag>);
    static constexpr auto value = Flags::value;
  };
};

using ready = meta_object<ready_initializer>;
using deferred = meta_object<deferred_initializer>;
using ready_ret = meta_ret_object<ready_initializer, identity>;
using initialized = meta_invoke<ready>;

template<class Object>
using observed = meta_object_invoke<
    meta_states_object<7, inspect_original<Object>>, Object, argument_tag>;

static_assert(observed<ready>::value == 7);
static_assert(observed<deferred>::value == 7);
static_assert(observed<ready_ret>::value == 7);
static_assert(observed<initialized>::value == 7);

using ordinary = meta_object<void, identity>;
static_assert(std::is_same_v<meta_object_invoke<ordinary, ready>::type, void>);
using pending = meta_object_invoke<ordinary, deferred>;
static_assert(std::is_same_v<meta_invoke<pending, int>::type, void>);

struct read_flags {
  template<class Flags, class OtherFlags>
  using apply = OtherFlags;
};
using states_result = meta_object_invoke<meta_states_object<0, read_flags>,
                                         meta_states_object<9, read_flags>>;
static_assert(states_result::value == 9);

int main() {}