#pragma once

#include "winapi_ui.hpp"

#if defined(_WIN32)
namespace winapi_ui_static {

using namespace winapi_ui;

namespace composition_detail {
template <class Visitor>
struct composition {
  Visitor visit;
};

template <class Component>
auto describe(Component&& component) {
  return composition{[value = std::forward<Component>(component)](auto&& consume) mutable
      -> decltype(auto) { return consume(std::move(value)); }};
}

template <class Visitor>
auto describe(composition<Visitor> component) { return component; }

template <class Left, class Right>
auto operator+(composition<Left> left, composition<Right> right) {
  return composition{[left = std::move(left), right = std::move(right)](
      auto&& consume) mutable -> decltype(auto) {
    return left.visit([&](auto&&... first) -> decltype(auto) {
      return right.visit([&](auto&&... second) -> decltype(auto) {
        return consume(std::forward<decltype(first)>(first)...,
                       std::forward<decltype(second)>(second)...);
      });
    });
  }};
}
}

template <class... Components> requires(sizeof...(Components) > 0)
auto link(Components&&... components) {
  if constexpr ((requires {
      typename std::remove_cvref_t<Components>::argument_types;
    } && ...)) {
    return meta_bind::link_static_binders(std::forward<Components>(components)...);
  } else {
    auto description = (composition_detail::describe(
        std::forward<Components>(components)) + ...);
    return description.visit([](auto&&... flat) {
      return meta_bind::link_static_binders(std::forward<decltype(flat)>(flat)...);
    });
  }
}

template <class... Actions> requires(sizeof...(Actions) > 0)
auto with_child(int id, Actions&&... actions) {
  auto enter = meta_bind::bind_as<HWND(HWND, int)>(
      [](HWND parent, int identifier) { return winapi_ui::child(parent, identifier); });
  enter.bind_a_lot(nullptr, id);
  enter.as_front_feed();
  auto leave = meta_bind::bind_as<HWND(HWND)>([](HWND target) {
    return winapi_ui::require_window(GetParent(winapi_ui::require_window(target)));
  });
  leave.bind(nullptr);
  leave.as_front_feed();
  return (composition_detail::describe(std::move(enter)) + ... +
      composition_detail::describe(std::forward<Actions>(actions))) +
      composition_detail::describe(std::move(leave));
}

template <class... Components> requires(sizeof...(Components) > 0)
auto run(HWND target, Components&&... components) {
  auto chain = winapi_ui_static::link(std::forward<Components>(components)...);
  chain.template write<0>(winapi_ui::require_window(target));
  chain.execute_feed();
  return chain.returns().template get<decltype(chain)::stage_count - 1>();
}

inline auto window() {
  return winapi_ui_static::link(register_class(), create_window(), show_window(),
                                message_loop());
}

inline auto window_without_loop() {
  return winapi_ui_static::link(register_class(), create_window(), show_window());
}

}
#endif