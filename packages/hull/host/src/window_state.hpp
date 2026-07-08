#pragma once
// Opt-in window-state persistence (.hullrc window.rememberState -> --remember-state):
// the host samples the window's bounds/maximized/fullscreen once a second on the UI
// thread and writes them to <app data dir>/window-state.json when the app exits;
// the next launch restores them. Plain JSON, own file — deliberately not entangled
// with the (possibly encrypted) settings store.
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

#include "paths.hpp"
#include "window_ctl.hpp"

namespace window_state {

struct State {
  bool valid = false;
  bool has_position = false;
  int x = 0, y = 0, width = 0, height = 0;
  bool maximized = false;
  bool fullscreen = false;
};

inline fs::path state_file() { return storage::app_data_dir() / "window-state.json"; }

inline State load() {
  State s;
  std::ifstream in(state_file());
  if (!in) return s;
  try {
    nlohmann::json j = nlohmann::json::parse(in);
    s.width = j.value("width", 0);
    s.height = j.value("height", 0);
    if (s.width < 200 || s.height < 200) return s; // sanity: refuse degenerate sizes
    s.has_position = j.contains("x") && j.contains("y");
    s.x = j.value("x", 0);
    s.y = j.value("y", 0);
    s.maximized = j.value("maximized", false);
    s.fullscreen = j.value("fullscreen", false);
    s.valid = true;
  } catch (...) { /* corrupt file -> defaults */ }
  return s;
}

inline void save(const State& s) {
  if (!s.valid) return;
  nlohmann::json j{{"width", s.width}, {"height", s.height},
                   {"maximized", s.maximized}, {"fullscreen", s.fullscreen}};
  if (s.has_position) { j["x"] = s.x; j["y"] = s.y; }
  std::ofstream out(state_file(), std::ios::trunc);
  if (out) out << j.dump(2);
  storage::lock_down(state_file());
}

// Sample the current state on the UI thread. Bounds are only captured while the
// window is in its normal state, so a maximized/fullscreen close still restores
// to sensible windowed bounds next launch.
inline void capture(void* handle, State& s) {
  if (!handle) return;
  const bool maximized = window_ctl::is_maximized(handle);
  const bool fullscreen = window_ctl::is_fullscreen(handle);
  s.maximized = maximized;
  s.fullscreen = fullscreen;
  if (!maximized && !fullscreen) {
    window_ctl::Bounds b;
    if (window_ctl::get_bounds(handle, b) && b.width >= 200 && b.height >= 200) {
      s.has_position = b.has_position;
      s.x = b.x; s.y = b.y;
      s.width = b.width; s.height = b.height;
      s.valid = true;
    }
  } else if (s.width == 0) {
    s.valid = s.valid || false;
  }
}

} // namespace window_state
