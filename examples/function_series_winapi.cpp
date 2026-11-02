// 把真实 WinAPI 绑进 function_binder，再用 function_series 串成调用链。
//
// 这个例子只做三件事：
//   1. 直接绑定 GetModuleHandleA / GetCurrentProcessId / CreateWindowExA
//      （12 个参数）并调用；
//   2. 用 link_f 把 GetModuleHandleA -> 适配 lambda -> DestroyWindow 串成一条
//      链，上一段返回值自动喂给下一段；每段返回值从 aligned_range 里取；
//   3. 用 drive()（底层 looper 状态机，一段一轮）和 run_loop()（重复执行）
//      各跑一遍同一条链。
//
// Windows 下 STATIC 是系统预注册窗口类，不建消息循环也能 CreateWindowExA /
// DestroyWindow，所以这个例子不弹窗、不阻塞。
//
// 编译（MinGW / MSVC 均可，本仓库只在 MinGW 上验证过）：
//   g++ -std=c++23 -O0 -I. -o series_winapi examples/function_series_winapi.cpp

#include "meta_function_series.hpp"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace winapi_demo {

#if defined(_WIN32)

using namespace meta_bind;

bool run() {
  bool ok = true;

  // ==========================================================================
  // 1) 直接绑定 WinAPI
  // ==========================================================================
  // GetModuleHandleA(LPCSTR) -> HMODULE
  auto get_module = meta_bind::bind(GetModuleHandleA);
  get_module.bind(nullptr);
  HMODULE module = get_module();
  ok = ok && module != nullptr;

  // GetCurrentProcessId() -> DWORD：零参数 binder
  auto get_pid = meta_bind::bind(GetCurrentProcessId);
  DWORD pid = get_pid();
  ok = ok && pid != 0;

  // CreateWindowExA 有 12 个参数，逐个 bind；STATIC 不需要 RegisterClass。
  auto create = meta_bind::bind(CreateWindowExA);
  create.bind(DWORD{0});                              // dwExStyle
  create.bind("STATIC");                              // lpClassName
  create.bind("meta_bind");                           // lpWindowName
  create.bind(DWORD{WS_OVERLAPPED});                  // dwStyle
  create.bind(0);                                     // X
  create.bind(0);                                     // Y
  create.bind(64);                                    // nWidth
  create.bind(32);                                    // nHeight
  create.bind(static_cast<HWND>(nullptr));            // hWndParent
  create.bind(static_cast<HMENU>(nullptr));           // hMenu
  create.bind(reinterpret_cast<HINSTANCE>(module));   // hInstance
  create.bind(static_cast<LPVOID>(nullptr));          // lpParam

  HWND direct_window = create();
  ok = ok && direct_window != nullptr;
  if (direct_window != nullptr) {
    ok = ok && DestroyWindow(direct_window) != 0;
  }

  std::printf("direct bind: module=%p pid=%lu window=%p\n",
              static_cast<void*>(module), static_cast<unsigned long>(pid),
              static_cast<void*>(direct_window));

  // ==========================================================================
  // 2) function_series：GetModuleHandleA -> CreateWindowExA 适配器 -> DestroyWindow
  // ==========================================================================
  auto series = meta_bind::link_f(
      GetModuleHandleA,  // LPCSTR -> HMODULE（真实 API）
      [](HMODULE instance) {  // HMODULE -> HWND
        return CreateWindowExA(
            DWORD{0}, "STATIC", "meta_bind series", DWORD{WS_OVERLAPPED}, 0, 0,
            96, 48, static_cast<HWND>(nullptr), static_cast<HMENU>(nullptr),
            reinterpret_cast<HINSTANCE>(instance),
            static_cast<LPVOID>(nullptr));
      },
      DestroyWindow);  // HWND -> BOOL（真实 API）

  series.bind(nullptr);  // 段 0 的参数
  series._bind_continue();

  // 每段的返回值都在这条 aligned_range 缓冲里：结构化绑定 / get<I> 都行。
  auto [series_module, series_window, series_destroyed] = series._return();
  ok = ok && series_module == module;
  ok = ok && series_window != nullptr;
  ok = ok && series_destroyed != 0;

  std::printf("series: module=%p window=%p destroyed=%d\n",
              static_cast<void*>(series_module),
              static_cast<void*>(series_window), static_cast<int>(series_destroyed));

  // ==========================================================================
  // 3) drive()：同一套链，改用 looper 状态机一段一轮地推进
  // ==========================================================================
  series.reset();
  series.bind(nullptr);
  series.drive();
  ok = ok && series._return<2>() != 0;
  std::printf("drive: window=%p destroyed=%d\n",
              static_cast<void*>(series._return<1>()),
              static_cast<int>(series._return<2>()));

  // ==========================================================================
  // 4) run_loop()：重复创建/销毁 5 个窗口，每次拿到本次的返回 range
  // ==========================================================================
  series.reset();
  series.bind(nullptr);
  std::size_t iterations = 0;
  std::size_t created = 0;
  std::size_t destroyed = 0;
  series.run_loop(5, [&](auto results) {
    ++iterations;
    if (results.template get<1>() != nullptr) ++created;
    if (results.template get<2>() != 0) ++destroyed;
  });
  ok = ok && iterations == 5 && created == 5 && destroyed == 5;

  std::printf("run_loop: iterations=%zu created=%zu destroyed=%zu\n",
              iterations, created, destroyed);

  // ==========================================================================
  // 5) 纯 API 链：GetCurrentProcessId -> std::to_string -> 取长度
  // ==========================================================================
  auto pid_series = meta_bind::link_f(
      GetCurrentProcessId,
      [](DWORD value) { return std::to_string(value); },
      [](std::string const& text) { return text.size(); });
  pid_series.bind();  // 零参数段：什么也不绑
  pid_series._bind_continue();
  ok = ok && pid_series._return<2>() == pid_series._return<1>().size();

  std::printf("pid series: '%s' length=%zu\n",
              pid_series._return<1>().c_str(),
              static_cast<std::size_t>(pid_series._return<2>()));

  return ok;
}

#else  // !_WIN32

bool run() {
  std::printf("winapi demo: skipped (not a Windows build)\n");
  return true;
}

#endif

}  // namespace winapi_demo

int main() {
  bool const ok = winapi_demo::run();
  std::printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
