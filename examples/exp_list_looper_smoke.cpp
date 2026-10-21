#include "meta_ios.hpp"

#include <type_traits>

namespace exp_list_looper_tests {

// Exercise eager exp_list aliases in every growing receiver state across chunks.
using loop = meta_states::run<
    meta_ios::stream_states_co<>, meta_iostream::meta_ostream<>,
    meta_iostream::index_sequence_istream<1000>>;
using result = loop::type::to_t;
static_assert(exp_utilities::exp_size<result> == 1000);
static_assert(result::front::value == 0);
static_assert(result::back::value == 999);
static_assert(result::at<500>::value == 500);
static_assert(exp_utilities::exp_size<result::pop_back> == 999);
static_assert(result::pop_back::back::value == 998);
static_assert(std::is_same_v<result::push_back<void>::pop_back, result>);

}  // namespace exp_list_looper_tests

int main() {}
