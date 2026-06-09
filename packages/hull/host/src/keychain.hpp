#pragma once
// OS keychain access (Windows Credential Manager / macOS Keychain / Linux libsecret).
// Webview-free so the crypto layer (secure.hpp) and DB (db_core.hpp) can fetch keys
// without pulling in the web view. The UI-facing bindings live in credentials.hpp.
#include <string>
#include <optional>
#include <stdexcept>

namespace secrets {

bool store(const std::string& service, const std::string& account, const std::string& secret);
std::optional<std::string> load(const std::string& service, const std::string& account);
bool erase(const std::string& service, const std::string& account);

// ----------------------------- Windows -----------------------------
#if defined(_WIN32)
} // namespace secrets
#include <windows.h>
#include <wincred.h>
namespace secrets {

inline std::wstring target(const std::string& service, const std::string& account) {
  std::string key = service + ":" + account;
  return std::wstring(key.begin(), key.end());
}

inline bool store(const std::string& service, const std::string& account,
                  const std::string& secret) {
  std::wstring t = target(service, account);
  CREDENTIALW cred{};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = const_cast<LPWSTR>(t.c_str());
  cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
  cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE; // per-user store; survives reboot
  return CredWriteW(&cred, 0) == TRUE;
}

inline std::optional<std::string> load(const std::string& service,
                                       const std::string& account) {
  std::wstring t = target(service, account);
  PCREDENTIALW pcred = nullptr;
  if (!CredReadW(t.c_str(), CRED_TYPE_GENERIC, 0, &pcred)) return std::nullopt;
  std::string secret(reinterpret_cast<char*>(pcred->CredentialBlob),
                     pcred->CredentialBlobSize);
  CredFree(pcred);
  return secret;
}

inline bool erase(const std::string& service, const std::string& account) {
  std::wstring t = target(service, account);
  return CredDeleteW(t.c_str(), CRED_TYPE_GENERIC, 0) == TRUE;
}

// ----------------------------- macOS -----------------------------
#elif defined(__APPLE__)
} // namespace secrets
#include <Security/Security.h>
namespace secrets {

inline bool store(const std::string& service, const std::string& account,
                  const std::string& secret) {
  erase(service, account); // replace if present
  OSStatus st = SecKeychainAddGenericPassword(
      nullptr,
      (UInt32)service.size(), service.c_str(),
      (UInt32)account.size(), account.c_str(),
      (UInt32)secret.size(),  secret.data(),
      nullptr);
  return st == errSecSuccess;
}

inline std::optional<std::string> load(const std::string& service,
                                       const std::string& account) {
  void* data = nullptr; UInt32 len = 0; SecKeychainItemRef item = nullptr;
  OSStatus st = SecKeychainFindGenericPassword(
      nullptr,
      (UInt32)service.size(), service.c_str(),
      (UInt32)account.size(), account.c_str(),
      &len, &data, &item);
  if (st != errSecSuccess) return std::nullopt;
  std::string secret(static_cast<char*>(data), len);
  SecKeychainItemFreeContent(nullptr, data);
  if (item) CFRelease(item);
  return secret;
}

inline bool erase(const std::string& service, const std::string& account) {
  void* data = nullptr; UInt32 len = 0; SecKeychainItemRef item = nullptr;
  OSStatus st = SecKeychainFindGenericPassword(
      nullptr,
      (UInt32)service.size(), service.c_str(),
      (UInt32)account.size(), account.c_str(),
      &len, &data, &item);
  if (st != errSecSuccess) return false;
  if (data) SecKeychainItemFreeContent(nullptr, data);
  bool ok = item && SecKeychainItemDelete(item) == errSecSuccess;
  if (item) CFRelease(item);
  return ok;
}

// ----------------------------- Linux (libsecret) -----------------------------
#else
} // namespace secrets
#include <libsecret/secret.h>
namespace secrets {

inline const SecretSchema* schema() {
  static const SecretSchema s = {
      "com.mwguerra.hull", SECRET_SCHEMA_NONE,
      {
          {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
      }};
  return &s;
}

inline bool store(const std::string& service, const std::string& account,
                  const std::string& secret) {
  GError* err = nullptr;
  gboolean ok = secret_password_store_sync(
      schema(), SECRET_COLLECTION_DEFAULT,
      (service + ":" + account).c_str(), // label
      secret.c_str(), nullptr, &err,
      "service", service.c_str(),
      "account", account.c_str(), nullptr);
  if (err) { g_error_free(err); return false; }
  return ok == TRUE;
}

inline std::optional<std::string> load(const std::string& service,
                                       const std::string& account) {
  GError* err = nullptr;
  gchar* pw = secret_password_lookup_sync(
      schema(), nullptr, &err,
      "service", service.c_str(),
      "account", account.c_str(), nullptr);
  if (err) { g_error_free(err); return std::nullopt; }
  if (!pw) return std::nullopt;
  std::string secret(pw);
  secret_password_free(pw);
  return secret;
}

inline bool erase(const std::string& service, const std::string& account) {
  GError* err = nullptr;
  gboolean ok = secret_password_clear_sync(
      schema(), nullptr, &err,
      "service", service.c_str(),
      "account", account.c_str(), nullptr);
  if (err) { g_error_free(err); return false; }
  return ok == TRUE;
}
#endif

} // namespace secrets
