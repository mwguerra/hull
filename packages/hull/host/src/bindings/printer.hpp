#pragma once
#include <string>
#include <vector>
#include <thread>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"

using json = nlohmann::json;

namespace printing {

struct Printer { std::string name; bool is_default = false; };

std::vector<Printer> list();
bool print_raw(const std::string& printer, const std::string& job_name,
               const std::string& bytes);
// Render plain text as a normal print job (GDI on Windows; text/plain via CUPS on
// macOS/Linux) so it works with ANY printer — Microsoft Print to PDF, OneNote, and
// physical laser printers — not just ESC/POS thermal printers. Single page, word-wrapped.
bool print_text(const std::string& printer, const std::string& job_name,
                const std::string& text);

// Build a minimal ESC/POS receipt: init, text lines, feed, full cut.
inline std::string escpos_message(const std::string& text) {
  std::string out;
  out += "\x1B\x40";          // ESC @  -> initialize
  out += text;
  out += "\n\n\n\n";          // feed
  out += "\x1D\x56\x00";      // GS V 0 -> full cut
  return out;
}

// ----------------------------- Windows -----------------------------
#if defined(_WIN32)
} // namespace printing
#include <windows.h>
#include <winspool.h>
namespace printing {

inline std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

inline std::vector<Printer> list() {
  std::vector<Printer> result;
  DWORD needed = 0, returned = 0;
  EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4,
                nullptr, 0, &needed, &returned);
  if (needed == 0) return result;
  std::vector<BYTE> buf(needed);
  if (EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4,
                    buf.data(), needed, &needed, &returned)) {
    auto* info = reinterpret_cast<PRINTER_INFO_4W*>(buf.data());
    for (DWORD i = 0; i < returned; ++i) {
      std::wstring w(info[i].pPrinterName);
      result.push_back({std::string(w.begin(), w.end()), false});
    }
  }
  // Mark the default printer.
  wchar_t def[256]; DWORD n = 256;
  if (GetDefaultPrinterW(def, &n)) {
    std::wstring w(def); std::string d(w.begin(), w.end());
    for (auto& p : result) if (p.name == d) p.is_default = true;
  }
  return result;
}

inline bool print_raw(const std::string& printer, const std::string& job_name,
                      const std::string& bytes) {
  std::wstring wprinter = utf8_to_wide(printer);
  HANDLE h = nullptr;
  if (!OpenPrinterW(const_cast<LPWSTR>(wprinter.c_str()), &h, nullptr)) return false;

  std::wstring wjob = utf8_to_wide(job_name);
  DOC_INFO_1W di{};
  di.pDocName = const_cast<LPWSTR>(wjob.c_str());
  di.pDatatype = const_cast<LPWSTR>(L"RAW");  // send bytes verbatim (ESC/POS or plain text)

  bool ok = false;
  if (StartDocPrinterW(h, 1, reinterpret_cast<LPBYTE>(&di))) {
    if (StartPagePrinter(h)) {
      DWORD written = 0;
      ok = WritePrinter(h, const_cast<char*>(bytes.data()),
                        (DWORD)bytes.size(), &written) && written == bytes.size();
      EndPagePrinter(h);
    }
    EndDocPrinter(h);
  }
  ClosePrinter(h);
  return ok;
}

inline bool print_text(const std::string& printer, const std::string& job_name,
                       const std::string& text) {
  const std::wstring wprinter = utf8_to_wide(printer);
  const std::wstring wjob = utf8_to_wide(job_name);
  const std::wstring wtext = utf8_to_wide(text);
  // GDI device context for the printer driver -> a real, rendered job (PDF/OneNote/laser).
  HDC dc = CreateDCW(L"WINSPOOL", wprinter.c_str(), nullptr, nullptr);
  if (!dc) return false;
  bool ok = false;
  DOCINFOW di{}; di.cbSize = sizeof(di); di.lpszDocName = wjob.c_str();
  if (StartDocW(dc, &di) > 0) {
    if (StartPage(dc) > 0) {
      const int dpiX = GetDeviceCaps(dc, LOGPIXELSX);
      const int dpiY = GetDeviceCaps(dc, LOGPIXELSY);
      HFONT font = CreateFontW(-MulDiv(11, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
      RECT r{ dpiX / 2, dpiY / 2,                                  // ~0.5" margins
              GetDeviceCaps(dc, HORZRES) - dpiX / 2,
              GetDeviceCaps(dc, VERTRES) - dpiY / 2 };
      DrawTextW(dc, wtext.c_str(), -1, &r,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
      if (oldFont) SelectObject(dc, oldFont);
      if (font) DeleteObject(font);
      ok = (EndPage(dc) > 0);
    }
    EndDoc(dc);
  }
  DeleteDC(dc);
  return ok;
}

// ----------------------------- macOS / Linux (CUPS) -----------------------------
#else
} // namespace printing
#include <cups/cups.h>
namespace printing {

inline std::vector<Printer> list() {
  std::vector<Printer> result;
  cups_dest_t* dests = nullptr;
  int n = cupsGetDests(&dests);
  for (int i = 0; i < n; ++i) {
    result.push_back({dests[i].name ? dests[i].name : "",
                      dests[i].is_default != 0});
  }
  cupsFreeDests(n, dests);
  return result;
}

inline bool print_raw(const std::string& printer, const std::string& job_name,
                      const std::string& bytes) {
  int job = cupsCreateJob(CUPS_HTTP_DEFAULT, printer.c_str(), job_name.c_str(),
                          0, nullptr);
  if (job == 0) return false;
  if (cupsStartDocument(CUPS_HTTP_DEFAULT, printer.c_str(), job, job_name.c_str(),
                        CUPS_FORMAT_RAW, 1) != HTTP_STATUS_CONTINUE)
    return false;
  cupsWriteRequestData(CUPS_HTTP_DEFAULT, bytes.data(), bytes.size());
  return cupsFinishDocument(CUPS_HTTP_DEFAULT, printer.c_str()) == IPP_STATUS_OK;
}

inline bool print_text(const std::string& printer, const std::string& job_name,
                       const std::string& text) {
  int job = cupsCreateJob(CUPS_HTTP_DEFAULT, printer.c_str(), job_name.c_str(), 0, nullptr);
  if (job == 0) return false;
  // CUPS_FORMAT_TEXT ("text/plain") -> CUPS renders via its text filter (any printer).
  if (cupsStartDocument(CUPS_HTTP_DEFAULT, printer.c_str(), job, job_name.c_str(),
                        CUPS_FORMAT_TEXT, 1) != HTTP_STATUS_CONTINUE)
    return false;
  cupsWriteRequestData(CUPS_HTTP_DEFAULT, text.data(), text.size());
  return cupsFinishDocument(CUPS_HTTP_DEFAULT, printer.c_str()) == IPP_STATUS_OK;
}
#endif

} // namespace printing

// ---- Portable raw-TCP sender (e.g. ESC/POS to a network printer on port 9100) ----
#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
#endif

inline bool print_to_socket(const std::string& host, int port, const std::string& bytes) {
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
  bool ok = false;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;        // IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0) {
    for (addrinfo* p = res; p; p = p->ai_next) {
#if defined(_WIN32)
      SOCKET fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd == INVALID_SOCKET) continue;
#else
      int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd < 0) continue;
#endif
      if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) {
        ok = true;
        size_t sent = 0;
        while (sent < bytes.size()) {
          int n = (int)send(fd, bytes.data() + sent, (int)(bytes.size() - sent), 0);
          if (n <= 0) { ok = false; break; }
          sent += (size_t)n;
        }
      }
#if defined(_WIN32)
      closesocket(fd);
#else
      ::close(fd);
#endif
      if (ok) break;
    }
    freeaddrinfo(res);
  }
#if defined(_WIN32)
  WSACleanup();
#endif
  return ok;
}

// ---- Bindings ----
inline void register_printer_bindings(Dispatcher& d) {
  // listPrinters() -> { ok, printers: [{name, isDefault}] }
  d.on("listPrinters", [](const json&, Reply reply) {
    json arr = json::array();
    for (const auto& p : printing::list())
      arr.push_back({{"name", p.name}, {"isDefault", p.is_default}});
    reply(json{{"ok", true}, {"printers", arr}});
  });

  // printMessage(printer, text) -> { ok }
  // Prints `text` as a normal text document, so it works with ANY printer (Microsoft
  // Print to PDF, OneNote, physical laser printers). For ESC/POS thermal receipt
  // printers use printReceipt / printNetwork instead.
  d.on("printMessage", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      json out;
      try {
        bool ok = printing::print_text(a.at(0).get<std::string>(), "App message",
                                       a.at(1).get<std::string>());
        out = {{"ok", ok}};
      } catch (const std::exception& e) { out = {{"ok", false}, {"error", e.what()}}; }
      reply(out);
    }).detach();
  });

  // printReceipt(printer, text) -> { ok }
  // Raw ESC/POS to a local (spooler) thermal/receipt printer: init, text, feed, cut.
  d.on("printReceipt", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      json out;
      try {
        bool ok = printing::print_raw(a.at(0).get<std::string>(), "App receipt",
                                      printing::escpos_message(a.at(1).get<std::string>()));
        out = {{"ok", ok}};
      } catch (const std::exception& e) { out = {{"ok", false}, {"error", e.what()}}; }
      reply(out);
    }).detach();
  });

  // printNetwork(host, port, text) -> { ok }
  d.on("printNetwork", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      json out;
      try {
        bool ok = print_to_socket(a.at(0).get<std::string>(), a.at(1).get<int>(),
                                  printing::escpos_message(a.at(2).get<std::string>()));
        out = {{"ok", ok}};
      } catch (const std::exception& e) { out = {{"ok", false}, {"error", e.what()}}; }
      reply(out);
    }).detach();
  });
}
