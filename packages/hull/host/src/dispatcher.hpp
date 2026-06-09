#pragma once
// Transport-agnostic bridge core. Bindings register handlers here; the same handlers
// are then exposed over the webview (native build) AND over HTTP/SSE (browser dev
// mode). Webview-free on purpose.
//
//   Handler: (args json) -> reply(result json)   — reply may be called sync or async.
//   emit():  C++ -> UI push (settings:changed, etc.); also feeds the dev trace.
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

using Reply = std::function<void(const json& result)>;
using Handler = std::function<void(const json& args, Reply reply)>;

class Dispatcher {
public:
  void on(const std::string& name, Handler h) { handlers_[name] = std::move(h); }
  bool has(const std::string& name) const { return handlers_.count(name) != 0; }

  std::vector<std::string> names() const {
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (const auto& kv : handlers_) out.push_back(kv.first);
    return out;
  }

  // Where C++ -> UI pushes go (webview eval, or SSE broadcast). Set by the transport.
  void set_emit_sink(std::function<void(const std::string&, const json&)> sink) {
    emit_sink_ = std::move(sink);
  }
  // Enable the dev trace (every call/reply/event mirrored on the "__trace" event).
  void set_trace(bool on) { trace_ = on; }
  bool tracing() const { return trace_; }

  // C++ -> UI push.
  void emit(const std::string& event, const json& payload) {
    if (trace_ && event != "__trace") {
      raw_emit("__trace", {{"type", "event"}, {"event", event}, {"payload", payload}, {"t", now_ms()}});
    }
    raw_emit(event, payload);
  }

  // Invoke a handler by name. `reply` is called exactly once (sync or async).
  void invoke(const std::string& name, const json& args, Reply reply) {
    auto it = handlers_.find(name);
    if (it == handlers_.end()) {
      reply(json{{"ok", false}, {"error", "unknown binding: " + name}});
      return;
    }
    if (!trace_) { it->second(args, reply); return; }

    const double t0 = now_ms();
    static long long seq = 0;
    const long long id = ++seq;
    raw_emit("__trace", {{"type", "call"}, {"id", id}, {"name", name}, {"args", args}, {"t", t0}});
    it->second(args, [this, name, id, t0, reply](const json& res) {
      raw_emit("__trace", {{"type", "reply"}, {"id", id}, {"name", name},
                           {"ok", res.value("ok", true)}, {"result", res},
                           {"durMs", now_ms() - t0}});
      reply(res);
    });
  }

private:
  void raw_emit(const std::string& event, const json& payload) {
    if (emit_sink_) emit_sink_(event, payload);
  }
  static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
  }

  std::map<std::string, Handler> handlers_;
  std::function<void(const std::string&, const json&)> emit_sink_ = nullptr;
  bool trace_ = false;
};
