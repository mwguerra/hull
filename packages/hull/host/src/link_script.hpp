#pragma once
// Link policy + origin guard injected into the app web view (webview::init —
// runs at document start on EVERY navigation, before any page script).
//
// Two layers:
//
// 1. ORIGIN GUARD (the security boundary). The native bindings are attached to
//    window.* with no origin check, and click interception alone cannot stop
//    programmatic navigation (location.href, form submits, meta refresh). So on
//    every document that is NOT the app's own origin, this script — which runs
//    after the binding wrappers and before page scripts — strips every binding,
//    the webview RPC object, and the emit hook. A remote page in the bridged
//    window gets a plain web page, nothing more.
//
// 2. LINK POLICY (the UX default) — only on the app's own origin:
//      external http(s)/mailto/tel  -> OS default browser/handler (openExternal)
//      <a data-hull-window>         -> NEW Hull window (openWindow, --no-bridge)
//      <a data-hull-ignore>         -> untouched (web view default behavior)
//      same-origin / #hash / download -> untouched (normal SPA navigation)
//      window.open(external)        -> OS default browser, returns null
//      window.open(anything else)   -> no-op (use openWindow() for app popups)
//    Handles HTML <a>, SVG <a> (SVGAnimatedString hrefs), <area>, and both
//    click and middle-click (auxclick).
//
// Never injected into --no-bridge child windows: those have no bindings at all.
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace link_script {

// `app_origin` is "file:" for packaged apps (--app) or the scheme://host[:port]
// of the dev server (--url). `binding_names` is every dispatcher binding — the
// exact set the origin guard must strip on foreign pages.
inline std::string build(const std::string& app_origin,
                         const std::vector<std::string>& binding_names) {
  const std::string origin_js = nlohmann::json(app_origin).dump();
  const std::string names_js = nlohmann::json(binding_names).dump();

  std::string js = "(function () {\n\"use strict\";\n";
  js += "var APP_ORIGIN = " + origin_js + ";\n";
  js += "var BINDINGS = " + names_js + ";\n";
  js += R"HULLJS(
  var home = APP_ORIGIN === "file:"
      ? location.protocol === "file:"
      : location.origin.toLowerCase() === APP_ORIGIN.toLowerCase();

  // ---- origin guard: foreign page in the bridged window -> no bridge at all ----
  if (!home) {
    for (var i = 0; i < BINDINGS.length; i++) {
      try { delete window[BINDINGS[i]]; } catch (e) { /* best effort */ }
      try { window[BINDINGS[i]] = undefined; } catch (e) { /* best effort */ }
    }
    try { delete window.__webview__; } catch (e) { /* best effort */ }
    try { window.__webview__ = undefined; } catch (e) { /* best effort */ }
    try { delete window.__bridgeEmit; } catch (e) { /* best effort */ }
    try { window.__bridgeEmit = undefined; } catch (e) { /* best effort */ }
    return; // and no link policy: a foreign page is just a page
  }

  // ---- link policy (app origin only) ----
  if (window.__hullLinkPolicy) return;
  window.__hullLinkPolicy = true;

  function abs(href) {
    try { return new URL(href, location.href); } catch (e) { return null; }
  }
  // External = handed to the OS (default browser / mail client / dialer).
  function isExternal(u) {
    if (!u) return false;
    if (u.protocol === "mailto:" || u.protocol === "tel:") return true;
    if (u.protocol !== "http:" && u.protocol !== "https:") return false;
    return u.origin !== location.origin; // file:// app => every http(s) link is external
  }
  function isWeb(u) { return u && (u.protocol === "http:" || u.protocol === "https:"); }
  function toExternal(url) {
    if (typeof window.openExternal === "function") { window.openExternal(url); return true; }
    return false;
  }
  function toHullWindow(url) {
    if (typeof window.openWindow === "function") { window.openWindow(url, {}); return true; }
    return false;
  }
  // HTML <a>, SVG <a> (nodeName "a", href is an SVGAnimatedString), and <area>.
  function hrefOf(n) {
    if (!n || !n.nodeName) return null;
    var name = n.nodeName.toUpperCase();
    if (name !== "A" && name !== "AREA") return null;
    var href = n.href;
    if (href && typeof href === "object") href = href.baseVal; // SVGAnimatedString
    if (!href && n.getAttribute) href = n.getAttribute("href") || n.getAttribute("xlink:href");
    return href ? String(href) : null;
  }
  function linkFor(ev) {
    var path = ev.composedPath ? ev.composedPath() : [];
    for (var i = 0; i < path.length; i++) {
      var h = hrefOf(path[i]);
      if (h) return { el: path[i], href: h };
    }
    var el = ev.target;
    while (el && el !== document) {
      var h2 = hrefOf(el);
      if (h2) return { el: el, href: h2 };
      el = el.parentElement;
    }
    return null;
  }

  function onActivate(ev) {
    if (ev.defaultPrevented) return;
    if (ev.button !== 0 && ev.button !== 1) return; // left + middle click
    var link = linkFor(ev);
    if (!link) return;
    var a = link.el;
    if (a.hasAttribute("download") || a.hasAttribute("data-hull-ignore")) return;
    var u = abs(link.href);
    if (!u) return;
    if (a.hasAttribute("data-hull-window")) {      // opt-in: new Hull window
      if (isWeb(u)) { ev.preventDefault(); toHullWindow(u.href); }
      return;
    }
    if (isExternal(u) && toExternal(u.href)) ev.preventDefault();
  }
  document.addEventListener("click", onActivate, true);
  document.addEventListener("auxclick", onActivate, true); // middle-click

  // window.open: external -> OS browser; everything else is a no-op. Returns
  // null always — a Hull app never gets an anonymous popup to script. For an
  // in-app popup, call openWindow(url) from @mwguerra/hull/bridge instead.
  window.open = function (url) {
    var u = url ? abs(String(url)) : null;
    if (u && isExternal(u)) { toExternal(u.href); return null; }
    if (u) console.warn("hull: window.open(" + u.href + ") suppressed — use openWindow() for a new Hull window");
    return null;
  };
})();)HULLJS";
  return js;
}

} // namespace link_script
