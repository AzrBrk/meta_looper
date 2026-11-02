#include "winapi_ui.hpp"
#include "winapi_series.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)

namespace bench {

using clock_type = std::chrono::steady_clock;
using word = std::uint32_t;
constexpr char class_name[] = "meta_chain_bench";
constexpr char window_title[] = "Chain bench";
constexpr std::size_t sample_count = 5;

template <class Value>
void opaque(Value& value) {
  asm volatile("" : "+m"(value) : : "memory");
}

struct result {
  std::string group;
  std::string name;
  std::size_t bytes;
  std::size_t stages;
  std::function<std::uint64_t(std::size_t)> run;
  std::array<double, sample_count> samples{};
  std::size_t iterations{1};
  double best{};
  double median{};
};

std::vector<result> results;
bool close_on_paint{};
bool display_verified{};
std::size_t painted_rows{};

template <class Runner>
void add(char const* group, char const* name, std::size_t bytes,
         std::size_t stages, Runner runner) {
  results.push_back({group, name, bytes, stages, std::move(runner)});
}

template <std::size_t Stage>
struct transform {
  word operator()(word value) const {
    return ((value ^ (0x9e3779b9u + static_cast<word>(Stage))) * 1664525u) +
           1013904223u;
  }
};

template <std::size_t... Stages>
word direct(word value, std::index_sequence<Stages...>) {
  ((value = transform<Stages>{}(value)), ...);
  return value;
}

template <std::size_t... Stages>
auto make_chain(std::index_sequence<Stages...>) {
  auto chain = meta_bind::link_f(transform<Stages>{}...);
  chain.template stage<0>().bind(word{1});
  return chain;
}

struct handler {
  virtual word call(word value) const = 0;
  virtual ~handler() = default;
};

template <std::size_t Stage>
struct virtual_transform final : handler {
  word call(word value) const override { return transform<Stage>{}(value); }
};

template <std::size_t... Stages>
void synthetic(std::index_sequence<Stages...> indices) {
  constexpr auto count = sizeof...(Stages);
  auto const group = count == 4 ? "A: 4 stages (ns/chain)" : "A: 8 stages (ns/chain)";
  auto chain = make_chain(indices);
  for (word input : {0u, 1u, 0x12345678u, 0xffffffffu}) {
    chain.reset();
    chain.bind(input);
    chain._bind_continue();
    if (chain.final_return() != direct(input, indices))
      throw std::runtime_error("synthetic unfolded mismatch");
    chain.reset();
    chain.bind(input);
    chain.drive();
    if (chain.final_return() != direct(input, indices))
      throw std::runtime_error("synthetic drive mismatch");
  }
  auto const loop = [](std::size_t iterations, auto&& call) {
    std::uint64_t checksum = 0;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
      word input = static_cast<word>(iteration);
      opaque(input);
      word output = call(input);
      opaque(output);
      checksum += output;
    }
    return checksum;
  };
  add(group, "direct", 0, count, [=](std::size_t iterations) {
    return loop(iterations, [=](word input) { return direct(input, indices); });
  });
  add(group, "struct wrapper", sizeof(transform<0>), count,
      [=](std::size_t iterations) {
        return loop(iterations, [=](word input) {
          ((input = transform<Stages>{}(input)), ...);
          return input;
        });
      });
  add(group, "series reset+feed", sizeof(chain), count,
      [=](std::size_t iterations) mutable {
        return loop(iterations, [&](word input) {
          chain.reset();
          chain.bind(input);
          opaque(chain);
          chain._bind_continue();
          return chain.final_return();
        });
      });
  add(group, "series static entry+feed", sizeof(chain), count,
      [=](std::size_t iterations) mutable {
        return loop(iterations, [&](word input) {
          chain.reset();
          chain.template stage<0>().template bind_at<0>(input);
          opaque(chain);
          chain.template at<0>()._bind_continue();
          return chain.final_return();
        });
      });
  add(group, "series reset+_continue", sizeof(chain), count,
      [=](std::size_t iterations) mutable {
        return loop(iterations, [&](word input) {
          chain.reset();
          (chain.template stage<Stages>().template bind_at<0>(
               direct(input, std::make_index_sequence<Stages>{})), ...);
            opaque(chain);
          chain._continue();
          return chain.final_return();
        });
      });
  add(group, "series reset+drive", sizeof(chain), count,
      [=](std::size_t iterations) mutable {
        return loop(iterations, [&](word input) {
          chain.reset();
          chain.bind(input);
          opaque(chain);
          chain.drive();
          return chain.final_return();
        });
      });
  add(group, "series rebuild+feed", sizeof(chain), count,
      [=](std::size_t iterations) {
        return loop(iterations, [&](word input) {
          auto fresh = make_chain(indices);
          fresh.template stage<0>().template bind_at<0>(input);
          opaque(fresh);
          fresh._bind_continue();
          return fresh.final_return();
        });
      });
  std::array<std::function<word(word)>, count> erased{transform<Stages>{}...};
  add(group, "std::function chain", sizeof(erased), count,
      [=](std::size_t iterations) {
        return loop(iterations, [&](word input) {
          for (auto const& function : erased) input = function(input);
          return input;
        });
      });
  std::array<std::shared_ptr<handler>, count> handlers{
      std::make_shared<virtual_transform<Stages>>()...};
  add(group, "virtual handler chain", sizeof(handlers), count,
      [=](std::size_t iterations) {
        return loop(iterations, [&](word input) {
          for (auto const& object : handlers) {
            auto pointer = object.get();
            opaque(pointer);
            input = pointer->call(input);
          }
          return input;
        });
      });
}

LRESULT CALLBACK test_proc(HWND window, UINT message, WPARAM wparam,
                           LPARAM lparam) {
  return DefWindowProcA(window, message, wparam, lparam);
}

HMODULE register_window() {
  HMODULE module = GetModuleHandleA(nullptr);
  if (!module) throw std::runtime_error("GetModuleHandleA failed");
  WNDCLASSA window_class{};
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = test_proc;
  window_class.hInstance = module;
  window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
  window_class.lpszClassName = class_name;
  if (!RegisterClassA(&window_class))
    throw std::runtime_error("RegisterClassA failed");
  return module;
}

HWND create(HMODULE module) {
  HWND window = CreateWindowExA(0, class_name, window_title, WS_POPUP,
                                0, 0, 320, 180, nullptr, nullptr, module, nullptr);
  if (!window) throw std::runtime_error("CreateWindowExA failed");
  return window;
}

HWND show(HWND window) {
  ShowWindow(window, SW_HIDE);
  UpdateWindow(window);
  return window;
}

void destroy(HWND window) {
  if (!DestroyWindow(window)) throw std::runtime_error("DestroyWindow failed");
}

struct window_wrapper {
  HMODULE reg() const { return register_window(); }
  HWND make(HMODULE module) const { return create(module); }
  HWND reveal(HWND window) const { return show(window); }
  void close(HWND window) const { destroy(window); }
};

struct window_context {
  HMODULE module{};
  HWND window{};
};

struct window_handler {
  virtual void call(window_context& context) const = 0;
  virtual ~window_handler() = default;
};

template <std::size_t Stage>
struct window_step final : window_handler {
  void call(window_context& context) const override {
    if constexpr (Stage == 0) context.module = register_window();
    if constexpr (Stage == 1) context.window = create(context.module);
    if constexpr (Stage == 2) show(context.window);
    if constexpr (Stage == 3) destroy(context.window);
  }
};

template <class Call>
std::uint64_t windows_loop(std::size_t iterations, Call&& call) {
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    call();
    if (!UnregisterClassA(class_name, GetModuleHandleA(nullptr)))
      throw std::runtime_error("UnregisterClassA failed");
  }
  return iterations;
}

auto ui_chain() {
  auto registration = winapi_ui::register_class();
  registration.set(winapi_ui::register_arg::class_name, class_name);
  registration.set(winapi_ui::register_arg::window_proc, &test_proc);
  auto creation = winapi_ui::create_window();
  creation.set(winapi_ui::create_arg::class_name, class_name);
  creation.set(winapi_ui::create_arg::title, window_title);
  creation.set(winapi_ui::create_arg::x, 0);
  creation.set(winapi_ui::create_arg::y, 0);
  creation.set(winapi_ui::create_arg::width, 320);
  creation.set(winapi_ui::create_arg::height, 180);
  creation.set(winapi_ui::create_arg::style, winapi_ui::window_style::popup);
  auto showing = winapi_ui::show_window();
  showing.set(winapi_ui::show_arg::command, winapi_ui::show_command::hide);
  return winapi_ui::link(std::move(registration), std::move(creation),
                        std::move(showing), winapi_ui::destroy_window());
}

void check_ui(auto const& chain) {
  if (!chain.template _return<0>() || !chain.template _return<1>() ||
      IsWindow(chain.template _return<1>()))
    throw std::runtime_error("UI lifecycle failed");
}

void real_windows() {
  constexpr char group[] = "B: full hidden lifecycle (us/window)";
  add(group, "direct WinAPI", 0, 4, [](std::size_t iterations) {
    return windows_loop(iterations, [] { destroy(show(create(register_window()))); });
  });
  add(group, "struct wrapper", sizeof(window_wrapper), 4,
      [](std::size_t iterations) {
        window_wrapper wrapper;
        return windows_loop(iterations, [&] {
          wrapper.close(wrapper.reveal(wrapper.make(wrapper.reg())));
        });
      });
  auto ui = ui_chain();
  add(group, "winapi_ui reused", sizeof(ui), 4,
      [ui](std::size_t iterations) mutable {
        return windows_loop(iterations, [&] {
          ui.reset();
          ui._bind_continue();
          check_ui(ui);
        });
      });
  add(group, "winapi_ui rebuilt", sizeof(ui), 4,
      [](std::size_t iterations) {
        return windows_loop(iterations, [] {
          auto fresh = ui_chain();
          fresh._bind_continue();
          check_ui(fresh);
        });
      });
  winapi_series::window_config config;
  config.class_name = class_name;
  config.title = window_title;
  config.window_proc = test_proc;
  config.style = WS_POPUP;
  config.x = 0;
  config.y = 0;
  config.width = 320;
  config.height = 180;
  config.show_command = SW_HIDE;
  auto series = meta_bind::series_cat(winapi_series::window_series(config),
                                     winapi_series::destroy_window_series());
  add(group, "winapi_series reused", sizeof(series), 5,
      [series](std::size_t iterations) mutable {
        return windows_loop(iterations, [&] {
          series.reset();
          series.bind(nullptr);
          series._bind_continue();
          if (!series.template _return<2>() || IsWindow(series.template _return<2>()))
            throw std::runtime_error("winapi_series lifecycle failed");
        });
      });
  std::array<std::function<void()>, 4> erased;
  auto context = std::make_shared<window_context>();
  erased = {[context] { context->module = register_window(); },
            [context] { context->window = create(context->module); },
            [context] { show(context->window); },
            [context] { destroy(context->window); }};
  add(group, "std::function chain", sizeof(erased), 4,
      [erased](std::size_t iterations) {
        return windows_loop(iterations, [&] {
          for (auto const& function : erased) function();
        });
      });
  std::array<std::shared_ptr<window_handler>, 4> handlers{
      std::make_shared<window_step<0>>(), std::make_shared<window_step<1>>(),
      std::make_shared<window_step<2>>(), std::make_shared<window_step<3>>()};
  add(group, "virtual handler chain", sizeof(handlers), 4,
      [handlers](std::size_t iterations) {
        window_context state;
        return windows_loop(iterations, [&] {
          for (auto const& object : handlers) object->call(state);
        });
      });
}

double timed(result& row, std::size_t iterations, std::uint64_t& checksum) {
  auto const start = clock_type::now();
  auto value = row.run(iterations);
  opaque(value);
  auto const elapsed = clock_type::now() - start;
  checksum ^= value;
  return std::chrono::duration<double, std::nano>(elapsed).count();
}

void measure() {
  std::uint64_t checksum{};
  for (auto& row : results) {
    std::uint64_t expected = 17;
    if (row.group[0] == 'A') {
      expected = 0;
      for (word input = 0; input < 17; ++input)
        expected += row.stages == 4 ? direct(input, std::make_index_sequence<4>{})
                                   : direct(input, std::make_index_sequence<8>{});
    }
    if (row.run(17) != expected) throw std::runtime_error("checksum mismatch");
    for (;;) {
      auto const ns = timed(row, row.iterations, checksum);
      if (ns >= 5000000.0) {
        row.iterations = std::max(std::size_t{1}, static_cast<std::size_t>(
            static_cast<double>(row.iterations) * 50000000.0 / ns));
        break;
      }
      row.iterations *= 2;
    }
  }
  for (std::size_t repeat = 0; repeat < sample_count; ++repeat) {
    for (std::size_t offset = 0; offset < results.size(); ++offset) {
      auto& row = results[(offset + repeat) % results.size()];
      row.samples[repeat] = timed(row, row.iterations, checksum) /
                            static_cast<double>(row.iterations);
    }
  }
  char const* previous = "";
  double baseline{};
  for (auto& row : results) {
    auto sorted = row.samples;
    std::sort(sorted.begin(), sorted.end());
    row.best = sorted.front();
    row.median = sorted[sample_count / 2];
    bool const lifecycle = row.group[0] == 'B';
    if (row.group != previous) {
      std::printf("\n%s\n%-28s %10s %10s %9s %8s %8s\n", row.group.c_str(),
                  "path", "best", "median", "ratio", "bytes", "ns/stage");
      previous = row.group.c_str();
      baseline = row.median;
    }
    double const divisor = lifecycle ? 1000.0 : 1.0;
    std::printf("%-28s %10.3f %10.3f %8.3fx %8zu %8.3f\n", row.name.c_str(),
                row.best / divisor, row.median / divisor, row.median / baseline,
                row.bytes, lifecycle ? 0.0 : row.median / row.stages);
  }
  std::printf("\nchecksum=%llu; five rotated samples, ~50ms each\n",
              static_cast<unsigned long long>(checksum));
  std::puts("Bytes exclude harness/heap allocations; baselines are dispatch shapes, not UI frameworks.");
  std::puts("_continue precomputes/binds each stage input; feed/drive propagate returns.");
  std::puts("Lifecycle includes unregister; winapi_series has an extra module stage.");
  std::puts("Small differences in lifecycle timings are noise, not isolated dispatch overhead.");
}

LRESULT CALLBACK display_proc(HWND window, UINT message, WPARAM wparam,
                              LPARAM lparam) {
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    auto old_font = SelectObject(dc, GetStockObject(ANSI_FIXED_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(30, 42, 42));
    int vertical = 14;
    std::string previous;
    double maximum = 1;
    for (auto const& row : results) {
      if (row.group != previous) {
        previous = row.group;
        TextOutA(dc, 16, vertical, row.group.c_str(), static_cast<int>(row.group.size()));
        vertical += 24;
        maximum = 1;
        for (auto const& other : results)
          if (other.group == row.group) maximum = std::max(maximum, other.median);
      }
      char line[160]{};
      std::snprintf(line, sizeof(line), "%-27s %9.3f %s  %4zu B", row.name.c_str(),
                    row.median / (row.group[0] == 'B' ? 1000.0 : 1.0),
                    row.group[0] == 'B' ? "us" : "ns", row.bytes);
      TextOutA(dc, 16, vertical, line, static_cast<int>(std::strlen(line)));
      RECT bar{510, vertical + 3,
               510 + static_cast<LONG>(std::max(1.0, std::max(0L, client.right - 530L) *
                                                              row.median / maximum)),
               vertical + 15};
      HBRUSH brush = CreateSolidBrush(row.group[0] == 'B' ? RGB(177, 82, 44) : RGB(20, 125, 113));
      FillRect(dc, &bar, brush);
      DeleteObject(brush);
      vertical += 21;
    }
    painted_rows = results.size();
    display_verified = IsWindowVisible(window) &&
                       GetPixel(dc, 510, 41) == RGB(20, 125, 113);
    SelectObject(dc, old_font);
    EndPaint(window, &paint);
    if (close_on_paint) PostMessageA(window, WM_CLOSE, 0, 0);
    return 0;
  }
  if (message == WM_GETMINMAXINFO) {
    auto limits = reinterpret_cast<MINMAXINFO*>(lparam);
    limits->ptMinTrackSize = {740, 710};
    return 0;
  }
  if (message == WM_SIZE) InvalidateRect(window, nullptr, TRUE);
  if (message == WM_KEYDOWN || message == WM_LBUTTONDOWN || message == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  return winapi_ui::default_window_proc(window, message, wparam, lparam);
}

int display() {
  auto window = winapi_ui::window();
  window.stage<0>().set(winapi_ui::register_arg::window_proc, &display_proc);
  window.stage<1>().set(winapi_ui::create_arg::title, "meta_looper | function-chain benchmark");
  window.stage<1>().set(winapi_ui::create_arg::width, 920);
  window.stage<1>().set(winapi_ui::create_arg::height, 740);
  window.stage<3>().set(winapi_ui::loop_arg::timer_ms, 5000u);
  window._bind_continue();
  if (!window._return<1>()) throw std::runtime_error("display window creation failed");
  if (!display_verified || painted_rows != results.size())
    throw std::runtime_error("display visibility/pixel/row validation failed");
  std::printf("Visible GDI window verified: %zu rows, bar pixel checked, %s exit\n",
              painted_rows, close_on_paint ? "WM_CLOSE" : "timer");
  return window.final_return();
}

}

int main(int argc, char** argv) {
  try {
    bench::close_on_paint = argc > 1 && std::strcmp(argv[1], "--close-on-paint") == 0;
    bench::synthetic(std::make_index_sequence<4>{});
    bench::synthetic(std::make_index_sequence<8>{});
    bench::real_windows();
    bench::measure();
    std::fflush(stdout);
    return argc > 1 && std::strcmp(argv[1], "--no-display") == 0 ? 0 : bench::display();
  } catch (std::exception const& error) {
    std::fprintf(stderr, "benchmark failed: %s (Win32=%lu)\n", error.what(), GetLastError());
    return 1;
  }
}

#else
int main() {
  std::puts("winapi_ui_chain_bench requires Windows");
  return 0;
}
#endif