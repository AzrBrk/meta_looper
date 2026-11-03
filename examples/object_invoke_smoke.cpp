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
static_assert(observed<meta_states_object<9, identity>>::value == 7);
static_assert(observed<meta_states_object<u128{9, 42}, identity>>::value == 7);

using ordinary = meta_object<void, identity>;
static_assert(std::is_same_v<meta_object_invoke<ordinary, ready>::type, void>);
using pending = meta_object_invoke<ordinary, deferred>;
static_assert(std::is_same_v<pending, meta_object_initialize<deferred>>);
static_assert(meta_object_t<pending>);
static_assert(uninitialized_meta_object_t<pending>);
static_assert(!initialized_meta_object_t<pending>);
static_assert(std::is_same_v<meta_produce_t<pending>, meta_uninitialized>);
static_assert(std::is_same_v<meta_invoke<pending, int>, meta_invoke<deferred, int>>);
static_assert(std::is_same_v<meta_produce_t<meta_invoke<pending, int>>, int>);

struct read_flags {
  template<class Flags, class Other>
  using apply = typename Other::flags;
};
using states_result = meta_object_invoke<meta_states_object<0, read_flags>,
                                         meta_states_object<9, read_flags>>;
static_assert(states_result::value == 9);
using data_states = meta_states_object<u128{9, 42}, identity>;
using ordinary_projection = meta_object<data_states, identity>;
struct capture_input {
  template<class Current, class Input>
  using apply = Input;
};
using receiver = meta_object<void, capture_input>;
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<receiver, deferred, int>>, int>);
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<receiver, pending, double>>, double>);

struct pair_initializer {
  template<class First, class Second>
  using initialize = exp_utilities::exp_list<First, Second>;
  template<class Current, class...>
  using apply = Current;
};
using pair_source = meta_object<pair_initializer>;
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<receiver, pair_source, int, double>>,
    exp_utilities::exp_list<int, double>>);

struct pointer_result {
  template<class Input>
  using apply = Input*;
};
using deferred_ret = meta_ret_object<deferred_initializer, pointer_result>;
using pending_ret = meta_object_invoke<receiver, deferred_ret>;
static_assert(uninitialized_meta_object_t<pending_ret>);
static_assert(std::is_same_v<meta_produce_t<pending_ret>, meta_uninitialized>);
static_assert(std::is_same_v<meta_invoke<pending_ret, int>, meta_invoke<deferred_ret, int>>);
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<receiver, deferred_ret, int>>, int*>);
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<receiver, pending_ret, double>>, double*>);

struct capture_inputs {
  template<class Current, class... Inputs>
  using apply = exp_utilities::exp_list<Inputs...>;
};
using collecting_receiver = meta_object<void, capture_inputs>;
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<collecting_receiver, ready, argument_tag, double>>,
    exp_utilities::exp_list<int, argument_tag, double>>);
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<collecting_receiver, ready_ret, argument_tag>>,
    exp_utilities::exp_list<int, argument_tag>>);
static_assert(std::is_same_v<
    meta_produce_t<meta_object_invoke<collecting_receiver, initialized, argument_tag>>,
    exp_utilities::exp_list<int, argument_tag>>);
static_assert(std::is_same_v<meta_object_initialize<initialized>, initialized>);
static_assert(std::is_same_v<meta_object_initialize<ready>, initialized>);

using projected = meta_object_invoke<meta_object<void, capture_input>, data_states>;
static_assert(std::is_same_v<projected::type, data_states::flags>);
static_assert(std::is_same_v<meta_produce_t<data_states>, data_states::flags>);
static_assert(observed<ordinary_projection>::value == 7);
static_assert(std::is_same_v<
    meta_object_invoke<meta_states_object<7, inspect_original<data_states>>,
                       data_states, argument_tag>,
    meta_invoke<meta_states_object<7, inspect_original<data_states>>,
                data_states, argument_tag>>);

struct needs_member {
  template<class Input>
  using apply = typename Input::member;
};
struct member_input {
  using member = int;
};
struct not_callable;

// 关闭分支不能实例化非法调用，甚至不要求目标满足元函数/元对象协议。
static_assert(std::is_same_v<
    meta_invoke<invoke_if<false>, needs_member, int>, needs_member>);
static_assert(std::is_same_v<
    meta_invoke<invoke_if<false>, not_callable, int>, not_callable>);
static_assert(std::is_same_v<
    meta_invoke<invoke_object_if<false>, not_callable, void>, not_callable>);
static_assert(std::is_same_v<
    meta_invoke<invoke_if<true>, needs_member, member_input>, int>);
static_assert(std::is_same_v<
    meta_invoke<invoke_object_if<false>, ordinary, deferred>, ordinary>);
static_assert(std::is_same_v<
    meta_invoke<invoke_object_if<true>, ordinary, ready>,
    meta_object_invoke<ordinary, ready>>);

int main() {}