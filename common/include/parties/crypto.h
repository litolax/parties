#pragma once

#include <string>
#include <cstdint>
#include <array>

struct WOLFSSL_CTX;
struct WOLFSSL;

namespace parties {

// Initialize/cleanup wolfSSL globally (call once)
bool crypto_init();
void crypto_cleanup();

// Generate cryptographically random bytes
void random_bytes(uint8_t* out, size_t len);

// Compute SHA-256 hash, return hex-with-colons string
std::string sha256_hex(const uint8_t* data, size_t len);

// Generate a self-signed RSA 4096 certificate + private key, write PEM files
bool generate_self_signed_cert(const std::string& common_name,
                               const std::string& cert_path,
                               const std::string& key_path);

// Create a TLS 1.3 server context with cert/key files
WOLFSSL_CTX* create_tls_server_ctx(const std::string& cert_path,
                                   const std::string& key_path);

// Create a TLS 1.3 client context (no CA verification — we use TOFU)
WOLFSSL_CTX* create_tls_client_ctx();

// Extract SHA-256 fingerprint of the peer certificate (hex with colons)
std::string get_peer_cert_fingerprint(WOLFSSL* ssl);

// Free a TLS context
void free_tls_ctx(WOLFSSL_CTX* ctx);

// Voice packet encryption (ChaCha20-Poly1305)
bool voice_encrypt(const uint8_t* key,          // 32 bytes
                   const uint8_t* nonce,         // 12 bytes
                   const uint8_t* aad, size_t aad_len,
                   const uint8_t* plaintext, size_t plaintext_len,
                   uint8_t* ciphertext,          // same size as plaintext
                   uint8_t* tag);                // 16 bytes

// Voice packet decryption (ChaCha20-Poly1305)
bool voice_decrypt(const uint8_t* key,           // 32 bytes
                   const uint8_t* nonce,          // 12 bytes
                   const uint8_t* aad, size_t aad_len,
                   const uint8_t* ciphertext, size_t ciphertext_len,
                   const uint8_t* tag,            // 16 bytes
                   uint8_t* plaintext);           // same size as ciphertext

} // namespace parties
