#pragma once

#include <array>
#include <cstdint>

namespace parties {

using UserId    = uint32_t;
using ChannelId = uint32_t;

using SessionToken = std::array<uint8_t, 32>;
using EnetToken    = std::array<uint8_t, 32>;
using ChannelKey   = std::array<uint8_t, 32>;

} // namespace parties
