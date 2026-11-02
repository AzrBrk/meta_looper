#include "winapi_ui_static.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
namespace {
using clock_type = std::chrono::steady_clock;
using word = std::uint32_t;
constexpr std::size_t sample_count = 7;

template <class Value>
void opaque(Value& value) { asm volatile("" : "+m"(value) : : "memory"); }

void require(bool condition) {
  if (!condition) throw std::runtime_error("benchmark correctness failure");
}

struct sample { double ns; std::uint64_t checksum; };
struct row {
  char const* name;
  std::size_t iterations;
  std::size_t bytes;
  std::function<sample(std::size_t)> run;
  std::array<double, sample_count> samples{};
};
std::vector<row> rows;

template <class Action>
sample timed(std::size_t iterations, Action&& action) {
  std::uint64_t checksum = 0;
  auto begin = clock_type::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    word input = static_cast<word>(iteration);
    opaque(input);
    word output = action(input);
    opaque(output);
    checksum += output;
  }
  auto end = clock_type::now();
  return {std::chrono::duration<double, std::nano>(end - begin).count() /
      static_cast<double>(iterations), checksum};
}

struct legacy {
  template <class... Components>
  static auto link(Components&&... components) {
    return winapi_ui::link(std::forward<Components>(components)...);
  }
  template <class Chain, class Input>
  static void input(Chain& chain, Input value) {
    chain.reset();
    chain.template stage<0>().template bind_at<0>(value);
  }
  template <class Chain>
  static auto execute(Chain& chain) {
    chain.template at<0>()._bind_continue();
    opaque(chain);
    return chain.final_return();
  }
  template <class... Actions>
  static auto child(int id, Actions&&... actions) {
    auto leave = meta_bind::bind_as<HWND(HWND)>([](HWND target) {
      return winapi_ui::require_window(GetParent(winapi_ui::require_window(target)));
    });
    leave.bind(nullptr);
    leave.as_front_feed();
    return winapi_ui::with_child(id, std::forward<Actions>(actions)...,
                                 std::move(leave));
  }
};

struct static_ui {
  template <class... Components>
  static auto link(Components&&... components) {
    return winapi_ui_static::link(std::forward<Components>(components)...);
  }
  template <class Chain, class Input>
  static void input(Chain& chain, Input value) { chain.template write<0>(value); }
  template <class Chain>
  static auto execute(Chain& chain) {
    chain.execute_feed();
    opaque(chain);
    return chain.returns().template get<Chain::stage_count - 1>();
  }
  template <class... Actions>
  static auto child(int id, Actions&&... actions) {
    return winapi_ui_static::with_child(id, std::forward<Actions>(actions)...);
  }
};

template <std::size_t Index>
word transform(word input, std::string text, word salt) {
  return (input ^ (0x9e3779b9u + static_cast<word>(Index))) * 1664525u + salt +
      static_cast<word>(text.size());
}

template <std::size_t Index>
HWND handle_transform(HWND input, std::string text, word salt) {
  auto token = transform<Index>(static_cast<word>(reinterpret_cast<std::uintptr_t>(input)),
                                std::move(text), salt);
  return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(token));
}

template <std::size_t Index>
auto component() {
  auto bound = meta_bind::bind(&handle_transform<Index>);
  bound.bind_a_lot(nullptr, std::string{"same default"}, word{1013904223u});
  bound.as_front_feed();
  return bound;
}

template <class Policy, std::size_t... Index>
auto synthetic_chain(std::index_sequence<Index...>) {
  return Policy::link(component<Index>()...);
}

template <std::size_t... Index>
word reference(word input, std::index_sequence<Index...>) {
  ((input = transform<Index>(input, "same default", 1013904223u)), ...);
  return input;
}

template <class Policy, std::size_t Count, bool Rebuild>
void add_synthetic(char const* name) {
  using indices = std::make_index_sequence<Count>;
  using chain_type = decltype(synthetic_chain<Policy>(indices{}));
  rows.push_back({name, Rebuild ? 100000u : 1000000u, sizeof(chain_type),
      [](std::size_t iterations) {
    auto chain = synthetic_chain<Policy>(indices{});
    auto measured = timed(iterations, [&](word input) {
      HWND token = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(input));
      if constexpr (Rebuild) {
        auto fresh = synthetic_chain<Policy>(indices{});
        Policy::input(fresh, token);
        opaque(fresh);
        return static_cast<word>(reinterpret_cast<std::uintptr_t>(Policy::execute(fresh)));
      } else {
        Policy::input(chain, token);
        opaque(chain);
        return static_cast<word>(reinterpret_cast<std::uintptr_t>(Policy::execute(chain)));
      }
    });
    std::uint64_t expected = 0;
    for (std::size_t index = 0; index < iterations; ++index)
      expected += reference(static_cast<word>(index), indices{});
    require(expected == measured.checksum);
    return measured;
  }});
}

struct window_owner {
  HWND handle = CreateWindowExA(0, "STATIC", "same root", WS_POPUP, 0, 0, 320, 200,
      nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  window_owner() { winapi_ui::require_window(handle); }
  ~window_owner() { if (handle) DestroyWindow(handle); }
};

template <class Policy>
auto controls() {
  return Policy::link(winapi_ui::edit(101, "Input"),
      winapi_ui::checkbox(102, "Enabled"), winapi_ui::combo_box(103),
      Policy::child(103, winapi_ui::combo_items({"One", "Two"}),
                         winapi_ui::combo_selection(0)));
}

template <class Policy>
auto actions() {
  return Policy::link(
      Policy::child(101, winapi_ui::set_text("Same updated text"),
                         winapi_ui::enable_window(true)),
      Policy::child(102, winapi_ui::check_box(true)),
      Policy::child(103, winapi_ui::combo_selection(1)));
}

void verify(HWND root) {
  require(winapi_ui::text(winapi_ui::child(root, 101)) == "Same updated text");
  require(IsWindowEnabled(winapi_ui::child(root, 101)));
  require(SendMessageA(winapi_ui::child(root, 102), BM_GETCHECK, 0, 0) == BST_CHECKED);
  require(SendMessageA(winapi_ui::child(root, 103), CB_GETCOUNT, 0, 0) == 2);
  require(SendMessageA(winapi_ui::child(root, 103), CB_GETCURSEL, 0, 0) == 1);
}

template <class Policy, bool Lifecycle, bool Rebuild = false>
void add_native(char const* name) {
  rows.push_back({name, Lifecycle ? 40u : 3000u,
      (Lifecycle ? sizeof(decltype(controls<Policy>())) : 0u) +
          sizeof(decltype(actions<Policy>())),
      [](std::size_t iterations) {
    if constexpr (Lifecycle) {
      return timed(iterations, [](word) {
        window_owner root;
        auto creation = controls<Policy>();
        Policy::input(creation, root.handle);
        opaque(creation);
        require(Policy::execute(creation) == root.handle);
        auto update = actions<Policy>();
        Policy::input(update, root.handle);
        opaque(update);
        require(Policy::execute(update) == root.handle);
        verify(root.handle);
        require(DestroyWindow(root.handle) != 0);
        root.handle = nullptr;
        return word{1};
      });
    } else {
      window_owner root;
      auto creation = controls<Policy>();
      Policy::input(creation, root.handle);
      require(Policy::execute(creation) == root.handle);
      auto update = actions<Policy>();
      auto measured = timed(iterations, [&](word) {
        if constexpr (Rebuild) {
          auto fresh = actions<Policy>();
          Policy::input(fresh, root.handle);
          opaque(fresh);
          require(Policy::execute(fresh) == root.handle);
        } else {
          Policy::input(update, root.handle);
          opaque(update);
          require(Policy::execute(update) == root.handle);
        }
        return word{1};
      });
      verify(root.handle);
      return measured;
    }
  }});
}
}

int main() {
  try {
    add_synthetic<legacy, 4, false>("synth4 reuse old");
    add_synthetic<static_ui, 4, false>("synth4 reuse static");
    add_synthetic<legacy, 4, true>("synth4 build old");
    add_synthetic<static_ui, 4, true>("synth4 build static");
    add_synthetic<legacy, 8, false>("synth8 reuse old");
    add_synthetic<static_ui, 8, false>("synth8 reuse static");
    add_synthetic<legacy, 8, true>("synth8 build old");
    add_synthetic<static_ui, 8, true>("synth8 build static");
    add_native<legacy, false>("native actions reuse old");
    add_native<static_ui, false>("native actions reuse static");
    add_native<legacy, false, true>("native actions build old");
    add_native<static_ui, false, true>("native actions build static");
    add_native<legacy, true>("native lifecycle old");
    add_native<static_ui, true>("native lifecycle static");
    for (auto& entry : rows) entry.run(3);
    for (std::size_t round = 0; round < sample_count; ++round) {
      for (std::size_t offset = 0; offset < rows.size(); ++offset) {
        auto index = (round + offset) % rows.size();
        auto& entry = rows[index];
        auto measured = entry.run(entry.iterations);
        if (index >= 8) require(measured.checksum == entry.iterations);
        entry.samples[round] = measured.ns;
      }
    }
    std::puts("GCC static UI comparison | ns/iteration | median of 7, rotating order");
    std::puts("Same strings/defaults/cached returns; barriers before and after execution.");
    std::puts("Synthetic HWND tokens are arithmetic only, never passed to WinAPI.");
    std::puts("Native: same GetDlgItem/GetParent calls; legacy uses real legacy nested owners.");
    std::puts("Reuse excludes setup; build includes construction/execution/destruction.");
    for (auto& entry : rows) {
      std::sort(entry.samples.begin(), entry.samples.end());
      auto checked = entry.run(entry.iterations);
      std::printf("%-29s median=%11.3f min=%11.3f max=%11.3f bytes=%4zu n=%zu checksum=%llu\n",
          entry.name, entry.samples[3], entry.samples.front(), entry.samples.back(),
          entry.bytes, entry.iterations, static_cast<unsigned long long>(checked.checksum));
    }
    return 0;
  } catch (std::exception const& error) {
    std::fprintf(stderr, "static_ui_compare: %s\n", error.what());
    return 1;
  }
}
#else
int main() { return 0; }
#endif