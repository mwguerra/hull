// Standalone test for the secure layer (default = NullCipher) + file store. Webview-
// free; no external libs. Build/run (default build):
//   g++ -std=c++17 -I../src secure_files_test.cpp -o t && ./t
#include <cstdio>
#include <cstdlib>
#include <string>
#include "secure.hpp"
#include "file_store.hpp"

static int failures = 0;
static void check(bool cond, const char* what) {
  std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) failures++;
}

int main() {
#if defined(_WIN32)
  _putenv_s("LOCALAPPDATA", (std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "\\hull-sectest").c_str());
#else
  setenv("XDG_DATA_HOME", "/tmp/hull-sectest", 1);
#endif
  storage::set_app_name("HullSecTest");

  // secure layer (default build = NullCipher, but self-describing format)
  check(secure::active() == false, "default build reports secure=false");
  {
    std::string blob = secure::encrypt("hello");
    check(!blob.empty() && (unsigned char)blob[0] == secure::TAG_PLAIN, "NullCipher tags plaintext (0x00)");
    auto back = secure::decrypt(blob);
    check(back && *back == "hello", "encrypt/decrypt round-trip");
  }
  {
    // A 0x01 (AES) blob can't be read by a default build -> clear error.
    bool threw = false;
    try { secure::decrypt(std::string(1, (char)secure::TAG_AES) + "junk"); }
    catch (const std::exception&) { threw = true; }
    check(threw, "default build refuses encrypted (0x01) data");
  }

  // base64 round-trip (incl. a NUL byte)
  {
    std::string raw = std::string("a\0b\xff\x10z", 6);
    check(appfiles::b64decode(appfiles::b64encode(raw)) == raw, "base64 round-trip (binary-safe)");
  }

  // file store round-trip + path-traversal protection
  try {
    appfiles::write_file("note.txt", "hello files");
    check(appfiles::read_file("note.txt") == "hello files", "file write/read round-trip");

    std::string bin = std::string("\x00\x01\x02\xfe\xff", 5);
    appfiles::write_file("blob.bin", bin);
    check(appfiles::read_file("blob.bin") == bin, "binary file round-trip");

    int rejected = 0;
    for (const char* bad : {"../escape", "a/b", "", ".."}) {
      try { appfiles::safe_name(bad); } catch (const std::exception&) { rejected++; }
    }
    check(rejected == 4, "safe_name rejects traversal / bad names");
  } catch (const std::exception& e) {
    std::printf("FAIL file store exception: %s\n", e.what());
    failures++;
  }

  std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
              failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
