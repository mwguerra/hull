#pragma once
// IMPORTANT: httplib before any webview/GTK/X11 headers (Linux macro clash).
#include <httplib.h>

#include <thread>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"
#include "keychain.hpp"

using json = nlohmann::json;

// Split a full URL into base ("https://host:port") and path ("/a/b?x=1").
inline std::pair<std::string, std::string> split_url(const std::string& url) {
  auto scheme = url.find("://");
  auto slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
  if (slash == std::string::npos) return {url, "/"};
  return {url.substr(0, slash), url.substr(slash)};
}

inline std::string host_of(const std::string& base) {
  auto scheme = base.find("://");
  std::string rest = scheme == std::string::npos ? base : base.substr(scheme + 3);
  auto colon = rest.find(':');
  return colon == std::string::npos ? rest : rest.substr(0, colon);
}

inline void register_http_bindings(Dispatcher& d) {
  // httpPost(url, body) -> { ok, status, body }
  d.on("httpPost", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      json out;
      try {
        const std::string url = a.at(0).get<std::string>();
        const json payload = a.at(1);
        auto [base, path] = split_url(url);
        httplib::Client cli(base);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        cli.enable_server_certificate_verification(true);
        httplib::Headers headers = {{"Accept", "application/json"}};
        if (auto token = secrets::load(host_of(base), "default"))
          headers.emplace("Authorization", "Bearer " + *token);
        auto res = cli.Post(path.c_str(), headers, payload.dump(), "application/json");
        if (!res) {
          out = {{"ok", false}, {"error", httplib::to_string(res.error())}};
        } else {
          json body;
          try { body = json::parse(res->body); } catch (...) { body = res->body; }
          out = {{"ok", res->status >= 200 && res->status < 300}, {"status", res->status}, {"body", body}};
        }
      } catch (const std::exception& e) { out = {{"ok", false}, {"error", e.what()}}; }
      reply(out);
    }).detach();
  });

  // httpGet(url) -> { ok, status, body }
  d.on("httpGet", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      json out;
      try {
        const std::string url = a.at(0).get<std::string>();
        auto [base, path] = split_url(url);
        httplib::Client cli(base);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        cli.enable_server_certificate_verification(true);
        httplib::Headers headers = {{"Accept", "application/json"}};
        if (auto token = secrets::load(host_of(base), "default"))
          headers.emplace("Authorization", "Bearer " + *token);
        auto res = cli.Get(path.c_str(), headers);
        if (!res) {
          out = {{"ok", false}, {"error", httplib::to_string(res.error())}};
        } else {
          json body;
          try { body = json::parse(res->body); } catch (...) { body = res->body; }
          out = {{"ok", res->status >= 200 && res->status < 300}, {"status", res->status}, {"body", body}};
        }
      } catch (const std::exception& e) { out = {{"ok", false}, {"error", e.what()}}; }
      reply(out);
    }).detach();
  });
}
