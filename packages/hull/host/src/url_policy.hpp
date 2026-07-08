#pragma once
// Pure URL / child-window policy — no platform calls, no dependencies, so it is
// unit-testable standalone (test/url_policy_test.cpp). The side-effecting half
// (actually opening URLs / spawning windows) lives in shell.hpp.
#include <cctype>
#include <string>
#include <vector>

namespace url_policy {

// RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), terminated by ":".
// Returns the lowercased scheme, or "" when the URL has none / is malformed.
inline std::string scheme_of(const std::string& url) {
  std::string s;
  for (char raw : url) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c == ':') return s;
    const bool ok = std::isalpha(c) ||
        (!s.empty() && (std::isdigit(c) || c == '+' || c == '-' || c == '.'));
    if (!ok) return std::string();
    s += static_cast<char>(std::tolower(c));
  }
  return std::string(); // no ':' -> no scheme
}

// URLs the host may hand to the OS (default browser / mail client / dialer).
// Everything else — file:, javascript:, data:, custom schemes — fails closed.
inline bool allowed_external_url(const std::string& url) {
  const std::string s = scheme_of(url);
  return s == "http" || s == "https" || s == "mailto" || s == "tel";
}

// URLs allowed in a NEW Hull web-view window: web content only.
inline bool allowed_window_url(const std::string& url) {
  const std::string s = scheme_of(url);
  return s == "http" || s == "https";
}

// argv for a child host window rendering `url`. --no-bridge is the security
// boundary: the child renders remote web content, which must never see the
// native bindings (database, keychain, files, ...).
inline std::vector<std::string> window_args(const std::string& exe, const std::string& url,
                                            const std::string& title, int width, int height) {
  return { exe, "--url", url, "--no-bridge",
           "--title", title,
           "--width", std::to_string(width),
           "--height", std::to_string(height) };
}

} // namespace url_policy
