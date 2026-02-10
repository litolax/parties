#pragma once

#include <string>

namespace parties::server {

// Argon2id parameters
constexpr uint32_t ARGON2_TIME_COST  = 3;       // iterations
constexpr uint32_t ARGON2_MEM_COST   = 65536;   // 64 MB
constexpr uint32_t ARGON2_PARALLELISM = 1;
constexpr uint32_t ARGON2_HASH_LEN   = 32;
constexpr uint32_t ARGON2_SALT_LEN   = 16;

// Hash a password with argon2id, returns encoded string (includes salt + params)
std::string hash_password(const std::string& password);

// Verify a password against an encoded argon2id hash
bool verify_password(const std::string& password, const std::string& encoded_hash);

} // namespace parties::server
