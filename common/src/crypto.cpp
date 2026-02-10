#include <parties/crypto.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/chacha20_poly1305.h>

#include <cstdio>
#include <fstream>
#include <vector>

namespace parties {

static WC_RNG g_rng;
static bool g_initialized = false;

bool crypto_init() {
    if (g_initialized) return true;
    wolfSSL_Init();
    if (wc_InitRng(&g_rng) != 0) return false;
    g_initialized = true;
    return true;
}

void crypto_cleanup() {
    if (!g_initialized) return;
    wc_FreeRng(&g_rng);
    wolfSSL_Cleanup();
    g_initialized = false;
}

void random_bytes(uint8_t* out, size_t len) {
    wc_RNG_GenerateBlock(&g_rng, out, static_cast<word32>(len));
}

std::string sha256_hex(const uint8_t* data, size_t len) {
    uint8_t hash[WC_SHA256_DIGEST_SIZE];
    wc_Sha256Hash(data, static_cast<word32>(len), hash);

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(WC_SHA256_DIGEST_SIZE * 3);
    for (int i = 0; i < WC_SHA256_DIGEST_SIZE; i++) {
        if (i > 0) result += ':';
        result += hex[(hash[i] >> 4) & 0xF];
        result += hex[hash[i] & 0xF];
    }
    return result;
}

bool generate_self_signed_cert(const std::string& common_name,
                               const std::string& cert_path,
                               const std::string& key_path) {
    // Heap-allocate RsaKey and Cert — both are very large structs in wolfSSL
    // (Cert alone can be 20KB+ with altNames) and will overflow the stack.
    auto* rsa = new RsaKey;
    wc_InitRsaKey(rsa, nullptr);

    // Generate RSA 4096 key
    if (wc_MakeRsaKey(rsa, 4096, WC_RSA_EXPONENT, &g_rng) != 0) {
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    // Buffer size for DER/PEM conversions — must be large enough for RSA 4096
    static constexpr word32 BUF_SZ = 16384;

    // Export private key to DER
    std::vector<uint8_t> key_der(BUF_SZ);
    int key_der_len = wc_RsaKeyToDer(rsa, key_der.data(), static_cast<word32>(key_der.size()));
    if (key_der_len <= 0) {
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    // Create certificate
    auto* cert = new Cert;
    wc_InitCert(cert);
    std::snprintf(cert->subject.commonName, CTC_NAME_SIZE, "%s", common_name.c_str());
    std::snprintf(cert->subject.org, CTC_NAME_SIZE, "%s", "Parties");
    cert->isCA = 0;
    cert->sigType = CTC_SHA256wRSA;
    cert->daysValid = 3650;

    // Use two-step approach: MakeCert + SignCert (more reliable than MakeSelfCert)
    std::vector<uint8_t> cert_der(BUF_SZ);

    int cert_body_len = wc_MakeCert(cert, cert_der.data(), BUF_SZ,
                                     rsa, nullptr, &g_rng);
    if (cert_body_len <= 0) {
        std::fprintf(stderr, "[Crypto] wc_MakeCert failed: %d\n", cert_body_len);
        delete cert;
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    int cert_der_len = wc_SignCert(cert->bodySz, cert->sigType,
                                    cert_der.data(), BUF_SZ,
                                    rsa, nullptr, &g_rng);
    delete cert;

    if (cert_der_len <= 0) {
        std::fprintf(stderr, "[Crypto] wc_SignCert failed: %d\n", cert_der_len);
        wc_FreeRsaKey(rsa);
        delete rsa;
        return false;
    }

    wc_FreeRsaKey(rsa);
    delete rsa;

    // Write DER files directly — avoids wolfSSL PEM code path issues
    std::ofstream cert_out(cert_path, std::ios::binary);
    if (!cert_out) return false;
    cert_out.write(reinterpret_cast<const char*>(cert_der.data()), cert_der_len);
    cert_out.close();

    std::ofstream key_out(key_path, std::ios::binary);
    if (!key_out) return false;
    key_out.write(reinterpret_cast<const char*>(key_der.data()), key_der_len);
    key_out.close();

    return true;
}

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

WOLFSSL_CTX* create_tls_server_ctx(const std::string& cert_path,
                                   const std::string& key_path) {
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (!ctx) {
        std::fprintf(stderr, "[Crypto] wolfSSL_CTX_new failed\n");
        return nullptr;
    }

    auto cert_data = read_file(cert_path);
    if (cert_data.empty()) {
        std::fprintf(stderr, "[Crypto] Failed to read cert file %s\n",
                     cert_path.c_str());
        wolfSSL_CTX_free(ctx);
        return nullptr;
    }

    if (wolfSSL_CTX_use_certificate_buffer(ctx, cert_data.data(),
            static_cast<long>(cert_data.size()),
            SSL_FILETYPE_ASN1) != SSL_SUCCESS) {
        std::fprintf(stderr, "[Crypto] Failed to load cert %s\n",
                     cert_path.c_str());
        wolfSSL_CTX_free(ctx);
        return nullptr;
    }

    auto key_data = read_file(key_path);
    if (key_data.empty()) {
        std::fprintf(stderr, "[Crypto] Failed to read key file %s\n",
                     key_path.c_str());
        wolfSSL_CTX_free(ctx);
        return nullptr;
    }

    if (wolfSSL_CTX_use_PrivateKey_buffer(ctx, key_data.data(),
            static_cast<long>(key_data.size()),
            SSL_FILETYPE_ASN1) != SSL_SUCCESS) {
        char buf[256];
        wolfSSL_ERR_error_string(wolfSSL_ERR_get_error(), buf);
        std::fprintf(stderr, "[Crypto] Failed to load key %s: %s\n",
                     key_path.c_str(), buf);
        wolfSSL_CTX_free(ctx);
        return nullptr;
    }

    return ctx;
}

WOLFSSL_CTX* create_tls_client_ctx() {
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!ctx) return nullptr;

    // No CA verification — we use TOFU
    wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    return ctx;
}

std::string get_peer_cert_fingerprint(WOLFSSL* ssl) {
    WOLFSSL_X509* cert = wolfSSL_get_peer_certificate(ssl);
    if (!cert) return "";

    int der_len = 0;
    const unsigned char* der = wolfSSL_X509_get_der(cert, &der_len);
    if (!der || der_len <= 0) return "";

    return sha256_hex(der, static_cast<size_t>(der_len));
}

void free_tls_ctx(WOLFSSL_CTX* ctx) {
    if (ctx) wolfSSL_CTX_free(ctx);
}

bool voice_encrypt(const uint8_t* key, const uint8_t* nonce,
                   const uint8_t* aad, size_t aad_len,
                   const uint8_t* plaintext, size_t plaintext_len,
                   uint8_t* ciphertext, uint8_t* tag) {
    return wc_ChaCha20Poly1305_Encrypt(
        key, nonce, aad, static_cast<word32>(aad_len),
        plaintext, static_cast<word32>(plaintext_len),
        ciphertext, tag) == 0;
}

bool voice_decrypt(const uint8_t* key, const uint8_t* nonce,
                   const uint8_t* aad, size_t aad_len,
                   const uint8_t* ciphertext, size_t ciphertext_len,
                   const uint8_t* tag, uint8_t* plaintext) {
    return wc_ChaCha20Poly1305_Decrypt(
        key, nonce, aad, static_cast<word32>(aad_len),
        ciphertext, static_cast<word32>(ciphertext_len),
        tag, plaintext) == 0;
}

} // namespace parties
