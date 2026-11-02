#include "meta_shared_series.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>

namespace {

struct mix {
  double operator()(int first, double second) const { return first * 0.5 + second; }
};

template <class T>
void materialize(T const& value) {
  asm volatile("" : : "m"(value) : "memory");
}

template <int Mode>
double run(std::size_t count) {
  double sum{};
  for (std::size_t index = 0; index < count; ++index) {
    int input = static_cast<int>(index & 255);
    materialize(input);
    if constexpr (Mode == 2) {
      auto chain = meta_bind::link_shared(mix{}, mix{});
      meta_bind::series_initialize(chain, input, 0.25, input, 0.5);
      materialize(chain);
      double first = chain.call<0>();
      double second = chain.call<1>();
      materialize(first);
      materialize(second);
      sum += first + second;
    } else {
      auto chain = meta_bind::link_f(mix{}, mix{});
      if constexpr (Mode == 0)
        meta_bind::series_bind(chain, input, 0.25, input, 0.5);
      else
        meta_bind::series_initialize(chain, input, 0.25, input, 0.5);
      materialize(chain);
      chain._continue();
      double first = chain._return<0>();
      double second = chain._return<1>();
      materialize(first);
      materialize(second);
      sum += first + second;
    }
  }
  return sum;
}

}

int main() {
  constexpr std::size_t count = 1u << 19;
  std::array<double (*)(std::size_t), 3> runs{run<0>, run<1>, run<2>};
  std::array names{"original bind", "original initialize", "shared initialize"};
  std::array<std::array<double, 7>, 3> samples{};
  double expected = count * (127.5 + 0.75);
  for (std::size_t repeat = 0; repeat < 7; ++repeat) {
    for (std::size_t step = 0; step < runs.size(); ++step) {
      auto mode = (step + repeat) % runs.size();
      auto start = std::chrono::steady_clock::now();
      double result = runs[mode](count);
      auto end = std::chrono::steady_clock::now();
      if (result != expected) return 1;
      samples[mode][repeat] =
          std::chrono::duration<double, std::nano>(end - start).count() / count;
    }
  }
  using original = decltype(meta_bind::link_f(mix{}, mix{}));
  using shared = decltype(meta_bind::link_shared(mix{}, mix{}));
  std::printf("original bytes=%zu shared bytes=%zu shared stack=%zu binder=%zu\n",
              sizeof(original), sizeof(shared), shared::stack_bytes,
              sizeof(shared::binder_type<0>));
  for (std::size_t mode = 0; mode < runs.size(); ++mode) {
    std::sort(samples[mode].begin(), samples[mode].end());
    std::printf("%-20s median %.3f ns/op\n", names[mode], samples[mode][3]);
  }
}