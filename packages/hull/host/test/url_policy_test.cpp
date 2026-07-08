// Standalone functional test for the link policy (url_policy.hpp). Dependency-free
// (no webview, no json). Run directly:
//   c++ -std=c++17 -I../src url_policy_test.cpp -o urltest && ./urltest
// or via the JS test runner: npm -w @mwguerra/hull run test (compiles it with cc).
#include <cstdio>
#include <string>
#include "url_policy.hpp"
#include "notify_text.hpp"

static int failures = 0;
static void check(bool cond, const char* what) {
  std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) failures++;
}

int main() {
  using namespace url_policy;

  // scheme extraction
  check(scheme_of("https://example.com") == "https", "scheme https");
  check(scheme_of("HTTPS://EXAMPLE.COM") == "https", "scheme lowercased");
  check(scheme_of("mailto:a@b.co") == "mailto", "scheme mailto");
  check(scheme_of("tel:+5521999999999") == "tel", "scheme tel");
  check(scheme_of("git+ssh://host/repo") == "git+ssh", "scheme with + preserved");
  check(scheme_of("/relative/path") == "", "relative path has no scheme");
  check(scheme_of("no-colon") == "", "no colon -> no scheme");
  check(scheme_of("9http://x") == "", "scheme must start with a letter");
  check(scheme_of(" https://padded") == "", "leading space invalidates the scheme");
  check(scheme_of("") == "", "empty string");
  check(scheme_of(":pathological") == "", "colon first -> no scheme");

  // external (OS handler) allowlist — fails closed
  check(allowed_external_url("https://example.com/x?y=1"), "https allowed externally");
  check(allowed_external_url("http://example.com"), "http allowed externally");
  check(allowed_external_url("mailto:someone@example.com"), "mailto allowed externally");
  check(allowed_external_url("tel:+15550100"), "tel allowed externally");
  check(!allowed_external_url("file:///etc/passwd"), "file: rejected");
  check(!allowed_external_url("javascript:alert(1)"), "javascript: rejected");
  check(!allowed_external_url("data:text/html,<script>"), "data: rejected");
  check(!allowed_external_url("smb://server/share"), "smb: rejected");
  check(!allowed_external_url("chrome://settings"), "chrome: rejected");
  check(!allowed_external_url(""), "empty rejected");
  check(!allowed_external_url("example.com"), "schemeless rejected");

  // new-Hull-window allowlist — web content only
  check(allowed_window_url("https://example.com"), "https allowed in a window");
  check(allowed_window_url("http://localhost:5173/"), "http allowed in a window");
  check(!allowed_window_url("mailto:a@b.co"), "mailto NOT allowed in a window");
  check(!allowed_window_url("tel:+15550100"), "tel NOT allowed in a window");
  check(!allowed_window_url("file:///app.html"), "file: NOT allowed in a window");

  // child-window argv: --no-bridge must ALWAYS be present (security boundary)
  {
    const auto args = window_args("/usr/bin/hull-host", "https://example.com/docs",
                                  "Docs", 900, 700);
    bool has_no_bridge = false, has_url = false;
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--no-bridge") has_no_bridge = true;
      if (args[i] == "--url" && i + 1 < args.size() && args[i + 1] == "https://example.com/docs")
        has_url = true;
    }
    check(args[0] == "/usr/bin/hull-host", "argv[0] is the exe");
    check(has_no_bridge, "child window argv carries --no-bridge");
    check(has_url, "child window argv carries --url <url>");
    check(args.size() == 10, "argv has exactly 10 entries");
  }

  // AppleScript escaping (macOS notification fallback) — quotes/backslashes only
  check(notify_text::applescript_escape("plain text") == "plain text", "escape: passthrough");
  check(notify_text::applescript_escape("say \"hi\"") == "say \\\"hi\\\"", "escape: quotes");
  check(notify_text::applescript_escape("back\\slash") == "back\\\\slash", "escape: backslash");
  check(notify_text::applescript_escape("") == "", "escape: empty");

  if (failures) { std::printf("\n%d FAILURE(S)\n", failures); return 1; }
  std::printf("\nall url_policy tests passed\n");
  return 0;
}
