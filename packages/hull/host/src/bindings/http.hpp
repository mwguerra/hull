#pragma once
// IMPORTANT: httplib before any webview/GTK/X11 headers (Linux macro clash).
#include <httplib.h>

#include <chrono>
#include <fstream>
#include <thread>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"
#include "keychain.hpp"
#include "../emit_hook.hpp"   // "http:download" progress events
#include "../paths.hpp"       // fs::path for downloads
#include "../file_store.hpp"  // appfiles::b64decode (multipart file parts)

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

// Shared client setup: TLS verification is ALWAYS on; keychain Bearer token is
// injected unless the caller opts out.
inline void configure_client(httplib::Client& cli, int timeout_ms, bool follow_redirects) {
  cli.set_connection_timeout(5);
  cli.set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
  cli.enable_server_certificate_verification(true);
  cli.set_follow_location(follow_redirects);
}

inline httplib::Headers build_headers(const std::string& base, const json& hdrs, bool auth) {
  httplib::Headers headers;
  bool has_accept = false;
  if (hdrs.is_object()) {
    for (auto it = hdrs.begin(); it != hdrs.end(); ++it) {
      if (it.value().is_string()) {
        headers.emplace(it.key(), it.value().get<std::string>());
        std::string k = it.key();
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (k == "accept") has_accept = true;
      }
    }
  }
  if (!has_accept) headers.emplace("Accept", "application/json");
  if (auth) {
    if (auto token = secrets::load(host_of(base), "default"))
      headers.emplace("Authorization", "Bearer " + *token);
  }
  return headers;
}

inline json response_json(const httplib::Result& res) {
  if (!res) return {{"ok", false}, {"error", httplib::to_string(res.error())}};
  json body;
  try { body = json::parse(res->body); } catch (...) { body = res->body; }
  json headers = json::object();
  for (const auto& [k, v] : res->headers) headers[k] = v;
  return {{"ok", res->status >= 200 && res->status < 300},
          {"status", res->status}, {"headers", headers}, {"body", body}};
}

inline void register_http_bindings(Dispatcher& d) {
  // httpRequest({ url, method?, headers?, body?, form?, timeoutMs?, auth?,
  //               followRedirects? }) -> { ok, status, headers, body }
  //   body: string (sent as-is; set a Content-Type header) or object/array
  //         (sent as application/json)
  //   form: [{name, value} | {name, fileName, contentBase64, contentType?}]
  //         (multipart/form-data, POST only)
  d.on("httpRequest", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      json out;
      try {
        const json o = (!a.empty() && a.at(0).is_object()) ? a.at(0) : json::object();
        const std::string url = o.value("url", "");
        std::string method = o.value("method", "GET");
        for (auto& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto [base, path] = split_url(url);
        httplib::Client cli(base);
        configure_client(cli, o.value("timeoutMs", 15000), o.value("followRedirects", true));
        httplib::Headers headers = build_headers(base, o.value("headers", json::object()),
                                                 o.value("auth", true));
        // Body: raw string (caller sets Content-Type) or JSON-encoded value.
        std::string body, content_type = "application/json";
        if (o.contains("body") && !o.at("body").is_null()) {
          if (o.at("body").is_string()) {
            body = o.at("body").get<std::string>();
            content_type = "text/plain";
            if (o.value("headers", json::object()).contains("Content-Type"))
              content_type = o.at("headers").at("Content-Type").get<std::string>();
          } else {
            body = o.at("body").dump();
          }
        }

        httplib::Result res;
        if (o.contains("form") && o.at("form").is_array()) {
          httplib::MultipartFormDataItems items;
          for (const auto& f : o.at("form")) {
            httplib::MultipartFormData item;
            item.name = f.value("name", "");
            if (f.contains("contentBase64")) {
              item.content = appfiles::b64decode(f.value("contentBase64", ""));
              item.filename = f.value("fileName", "file");
              item.content_type = f.value("contentType", "application/octet-stream");
            } else {
              item.content = f.value("value", "");
            }
            items.push_back(item);
          }
          res = cli.Post(path.c_str(), headers, items);
        } else if (method == "GET") res = cli.Get(path.c_str(), headers);
        else if (method == "HEAD") res = cli.Head(path.c_str(), headers);
        else if (method == "OPTIONS") res = cli.Options(path.c_str(), headers);
        else if (method == "POST") res = cli.Post(path.c_str(), headers, body, content_type.c_str());
        else if (method == "PUT") res = cli.Put(path.c_str(), headers, body, content_type.c_str());
        else if (method == "PATCH") res = cli.Patch(path.c_str(), headers, body, content_type.c_str());
        else if (method == "DELETE") {
          res = body.empty() ? cli.Delete(path.c_str(), headers)
                             : cli.Delete(path.c_str(), headers, body, content_type.c_str());
        } else {
          reply(json{{"ok", false}, {"error", "httpRequest: unsupported method " + method}});
          return;
        }
        out = response_json(res);
      } catch (const std::exception& e) { out = {{"ok", false}, {"error", e.what()}}; }
      reply(out);
    }).detach();
  });

  // httpDownload({ url, path, headers?, timeoutMs?, auth? }) -> { ok, path, bytes }
  // Streams to `path` (an absolute path, usually from dialogs.save) and emits
  // throttled "http:download" events: { url, path, received, total }.
  d.on("httpDownload", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      json out;
      try {
        const json o = (!a.empty() && a.at(0).is_object()) ? a.at(0) : json::object();
        const std::string url = o.value("url", "");
        const std::string dest = o.value("path", "");
        if (url.empty() || dest.empty()) {
          reply(json{{"ok", false}, {"error", "httpDownload: url and path are required"}});
          return;
        }
        auto [base, path] = split_url(url);
        httplib::Client cli(base);
        configure_client(cli, o.value("timeoutMs", 600000), true); // 10 min default
        httplib::Headers headers = build_headers(base, o.value("headers", json::object()),
                                                 o.value("auth", true));
        std::ofstream file(fs::path(dest), std::ios::binary | std::ios::trunc);
        if (!file) {
          reply(json{{"ok", false}, {"error", "httpDownload: cannot write " + dest}});
          return;
        }
        uint64_t received = 0;
        auto last_emit = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto res = cli.Get(
            path.c_str(), headers,
            [&](const char* data, size_t len) {
              file.write(data, static_cast<std::streamsize>(len));
              received += len;
              return file.good();
            },
            [&](uint64_t current, uint64_t total) {
              const auto now = std::chrono::steady_clock::now();
              if (now - last_emit >= std::chrono::milliseconds(150)) {
                last_emit = now;
                hooks::emit("http:download", {{"url", url}, {"path", dest},
                                              {"received", current}, {"total", total}});
              }
              return true;
            });
        file.close();
        if (!res || res->status < 200 || res->status >= 300) {
          std::error_code ec;
          fs::remove(fs::path(dest), ec); // don't leave partial files behind
          out = {{"ok", false},
                 {"status", res ? res->status : 0},
                 {"error", res ? ("HTTP " + std::to_string(res->status))
                               : httplib::to_string(res.error())}};
        } else {
          hooks::emit("http:download", {{"url", url}, {"path", dest},
                                        {"received", received}, {"total", received}});
          out = {{"ok", true}, {"path", dest}, {"bytes", received}};
        }
      } catch (const std::exception& e) { out = {{"ok", false}, {"error", e.what()}}; }
      reply(out);
    }).detach();
  });

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
