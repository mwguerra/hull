#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"
#include "keychain.hpp"   // secrets:: (webview-free keychain core)

// WRITE-ONLY from the UI. There is no "get secret" binding.
inline void register_credentials_bindings(Dispatcher& d) {
  // saveCredential(service, account, secret) -> { ok }
  d.on("saveCredential", [](const json& a, Reply reply) {
    try {
      bool ok = secrets::store(a.at(0).get<std::string>(),
                               a.at(1).get<std::string>(),
                               a.at(2).get<std::string>());
      reply(json{{"ok", ok}});
    } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
  });

  // credentialExists(service, account) -> { ok, exists }  (boolean only; never the secret)
  d.on("credentialExists", [](const json& a, Reply reply) {
    try {
      bool exists = secrets::load(a.at(0).get<std::string>(),
                                  a.at(1).get<std::string>()).has_value();
      reply(json{{"ok", true}, {"exists", exists}});
    } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
  });

  // eraseCredential(service, account) -> { ok }
  d.on("eraseCredential", [](const json& a, Reply reply) {
    try {
      bool ok = secrets::erase(a.at(0).get<std::string>(), a.at(1).get<std::string>());
      reply(json{{"ok", ok}});
    } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
  });
}
