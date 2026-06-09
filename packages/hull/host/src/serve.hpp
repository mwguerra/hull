#pragma once
// Browser dev-mode transport: exposes the dispatcher over HTTP (UI -> C++) and SSE
// (C++ -> UI). Reuses cpp-httplib (already linked) — no new dependency. Dev only.
//
//   POST /bridge/invoke   { name, args } -> result json
//   GET  /bridge/events   text/event-stream of { event, payload } (incl. __trace)
#include <httplib.h>   // before any webview/GTK/X11 headers
#include <string>
#include <memory>
#include <mutex>
#include <deque>
#include <set>
#include <optional>
#include <future>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"

using json = nlohmann::json;

// One connected Server-Sent-Events client: a thread-safe frame queue.
class SseClient {
public:
  void push(const std::string& frame) {
    { std::lock_guard<std::mutex> lk(m_); q_.push_back(frame); }
    cv_.notify_one();
  }
  void close() { closed_ = true; cv_.notify_all(); }
  // next frame, or "" on timeout (write a keepalive), or nullopt when closed.
  std::optional<std::string> pop(int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [&] { return !q_.empty() || closed_; })) {
      if (q_.empty()) return std::nullopt; // closed
      std::string f = q_.front(); q_.pop_front();
      return f;
    }
    return std::string(); // timeout -> keepalive
  }
private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::string> q_;
  std::atomic<bool> closed_{false};
};

class BridgeServer {
public:
  explicit BridgeServer(Dispatcher& d) : d_(d) {}

  // Push an event/trace frame to every connected SSE client (thread-safe).
  void broadcast(const std::string& event, const json& payload) {
    const std::string frame =
        "data: " + json{{"event", event}, {"payload", payload}}.dump() + "\n\n";
    std::lock_guard<std::mutex> lk(cm_);
    for (auto& c : clients_) c->push(frame);
  }

  void listen(const std::string& host, int port) {
    httplib::Server svr;

    // CORS (dev only) — the browser runs at the Vite origin, the host at another port.
    svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
      res.set_header("Access-Control-Allow-Origin", "*");
      res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });
    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
      res.set_content("ok", "text/plain");
    });

    // UI -> C++
    svr.Post("/bridge/invoke", [this](const httplib::Request& req, httplib::Response& res) {
      json body;
      try { body = json::parse(req.body); }
      catch (...) { res.status = 400; res.set_content(R"({"ok":false,"error":"bad json"})", "application/json"); return; }
      const std::string name = body.value("name", std::string());
      const json args = body.contains("args") ? body["args"] : json::array();
      std::promise<json> p;
      auto fut = p.get_future();
      d_.invoke(name, args, [&p](const json& result) { p.set_value(result); });
      res.set_content(fut.get().dump(), "application/json"); // blocks until the handler replies
    });

    // C++ -> UI (events + dev trace)
    svr.Get("/bridge/events", [this](const httplib::Request&, httplib::Response& res) {
      auto client = std::make_shared<SseClient>();
      { std::lock_guard<std::mutex> lk(cm_); clients_.insert(client); }
      res.set_chunked_content_provider(
          "text/event-stream",
          [client](size_t, httplib::DataSink& sink) {
            auto f = client->pop(15000);
            if (!f) return false; // closed
            const std::string frame = f->empty() ? std::string(": keepalive\n\n") : *f;
            return sink.write(frame.data(), frame.size());
          },
          [this, client](bool) {
            std::lock_guard<std::mutex> lk(cm_);
            clients_.erase(client);
          });
    });

    svr.listen(host.c_str(), port);
  }

private:
  Dispatcher& d_;
  std::mutex cm_;
  std::set<std::shared_ptr<SseClient>> clients_;
};
