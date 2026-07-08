#pragma once
// Process-wide C++ -> UI event hook. Native callbacks that fire outside a
// binding call (tray menu clicks, notification activation, second-instance
// pings) can't reach the Dispatcher directly — main.cpp installs it here once.
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace hooks {

inline std::function<void(const std::string&, const nlohmann::json&)>& emitter() {
  static std::function<void(const std::string&, const nlohmann::json&)> fn;
  return fn;
}

inline void emit(const std::string& event, const nlohmann::json& payload) {
  if (emitter()) emitter()(event, payload);
}

} // namespace hooks
