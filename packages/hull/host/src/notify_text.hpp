#pragma once
// Pure text helpers for notifications — no platform calls, unit-tested in
// test/url_policy_test.cpp (the standalone pure-helpers test).
#include <string>

namespace notify_text {

// Escape a string for embedding inside a double-quoted AppleScript literal
// (the macOS osascript fallback): backslashes and quotes get a backslash.
inline std::string applescript_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

} // namespace notify_text
