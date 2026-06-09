#pragma once
// Pluggable at-rest crypto layer for files + settings (the DB uses SQLCipher under
// the same flag — see db_core.hpp). Nothing else in the codebase calls crypto
// directly; everything goes through secure::encrypt / secure::decrypt.
//
//   Default build         -> NullCipher: passthrough, ZERO crypto cost (fast).
//   -DHULL_CRYPTO=ON      -> AES-256-GCM with a per-install key in the OS keychain.
//
// The on-disk blob is self-describing, so a secure build can still read old
// plaintext data, and a default build fails loudly on encrypted data:
//   byte 0 = 0x00  -> plaintext follows
//   byte 0 = 0x01  -> AES-256-GCM: [12-byte IV][16-byte tag][ciphertext]
#include <string>
#include <optional>
#include <stdexcept>

namespace secure {

inline constexpr unsigned char TAG_PLAIN = 0x00;
inline constexpr unsigned char TAG_AES = 0x01;

// true when real crypto is compiled in (the "secure" host build).
constexpr bool active() {
#if defined(HULL_CRYPTO)
  return true;
#else
  return false;
#endif
}

#if defined(HULL_CRYPTO)
// ============================ AES-256-GCM backend ============================
} // namespace secure
#include <vector>
#include "paths.hpp"      // storage::app_name (namespaces the key)
#include "keychain.hpp"   // secrets:: (per-install key in the OS keychain)
#include <openssl/evp.h>
#include <openssl/rand.h>
namespace secure {

inline std::vector<unsigned char> data_key() {
  // One random 32-byte key per install, stored in the keychain (namespaced by app).
  if (auto k = secrets::load(storage::app_name(), "secure-key")) {
    return std::vector<unsigned char>(k->begin(), k->end());
  }
  std::vector<unsigned char> key(32);
  RAND_bytes(key.data(), (int)key.size());
  secrets::store(storage::app_name(), "secure-key", std::string(key.begin(), key.end()));
  return key;
}

inline std::string encrypt(const std::string& plain) {
  auto key = data_key();
  std::vector<unsigned char> iv(12), tag(16), ct(plain.size() + 16);
  RAND_bytes(iv.data(), (int)iv.size());

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  int len = 0, ct_len = 0;
  EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)iv.size(), nullptr);
  EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data());
  EVP_EncryptUpdate(ctx, ct.data(), &len,
                    reinterpret_cast<const unsigned char*>(plain.data()), (int)plain.size());
  ct_len = len;
  EVP_EncryptFinal_ex(ctx, ct.data() + len, &len);
  ct_len += len;
  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)tag.size(), tag.data());
  EVP_CIPHER_CTX_free(ctx);

  std::string out;
  out.push_back((char)TAG_AES);
  out.append(reinterpret_cast<char*>(iv.data()), iv.size());
  out.append(reinterpret_cast<char*>(tag.data()), tag.size());
  out.append(reinterpret_cast<char*>(ct.data()), ct_len);
  return out;
}

inline std::optional<std::string> decrypt(const std::string& blob) {
  if (blob.empty()) return std::string();
  unsigned char tag0 = (unsigned char)blob[0];
  if (tag0 == TAG_PLAIN) return blob.substr(1);     // read legacy/plaintext too
  if (tag0 != TAG_AES || blob.size() < 1 + 12 + 16) return std::nullopt;
  auto key = data_key();
  const unsigned char* iv = reinterpret_cast<const unsigned char*>(blob.data() + 1);
  const unsigned char* tag = iv + 12;
  const unsigned char* ct = tag + 16;
  int ct_len = (int)blob.size() - 1 - 12 - 16;

  std::string out(ct_len, '\0');
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  int len = 0, out_len = 0;
  EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
  EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv);
  EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&out[0]), &len, ct, ct_len);
  out_len = len;
  EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char*>(tag));
  int ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]) + len, &len);
  EVP_CIPHER_CTX_free(ctx);
  if (ok <= 0) return std::nullopt; // tag mismatch -> tampered or wrong key
  out.resize(out_len + len);
  return out;
}

#else
// ============================ NullCipher (default) ============================
inline std::string encrypt(const std::string& plain) {
  std::string out;
  out.reserve(plain.size() + 1);
  out.push_back((char)TAG_PLAIN);
  out += plain;
  return out;
}

inline std::optional<std::string> decrypt(const std::string& blob) {
  if (blob.empty()) return std::string();
  unsigned char tag0 = (unsigned char)blob[0];
  if (tag0 == TAG_PLAIN) return blob.substr(1);
  throw std::runtime_error(
      "data is encrypted — rebuild the host with -DHULL_CRYPTO=ON (secure build)");
}
#endif

} // namespace secure
