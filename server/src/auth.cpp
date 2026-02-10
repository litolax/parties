#include <server/auth.h>
#include <parties/crypto.h>

#include <argon2.h>

#include <vector>
#include <cstdio>

namespace parties::server {

std::string hash_password(const std::string& password) {
    // Generate random salt
    uint8_t salt[ARGON2_SALT_LEN];
    parties::random_bytes(salt, sizeof(salt));

    // Calculate encoded length
    size_t encoded_len = argon2_encodedlen(ARGON2_TIME_COST, ARGON2_MEM_COST,
                                           ARGON2_PARALLELISM, ARGON2_SALT_LEN,
                                           ARGON2_HASH_LEN, Argon2_id);

    std::vector<char> encoded(encoded_len + 1, '\0');

    int rc = argon2id_hash_encoded(ARGON2_TIME_COST, ARGON2_MEM_COST,
                                    ARGON2_PARALLELISM,
                                    password.c_str(), password.size(),
                                    salt, sizeof(salt),
                                    ARGON2_HASH_LEN,
                                    encoded.data(),
                                    encoded_len);

    if (rc != ARGON2_OK) {
        std::fprintf(stderr, "[Auth] argon2id_hash_encoded failed: %s\n",
                     argon2_error_message(rc));
        return "";
    }

    return std::string(encoded.data());
}

bool verify_password(const std::string& password, const std::string& encoded_hash) {
    int rc = argon2id_verify(encoded_hash.c_str(),
                              password.c_str(), password.size());
    return rc == ARGON2_OK;
}

} // namespace parties::server
