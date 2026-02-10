#pragma once

#include <parties/types.h>
#include <parties/protocol.h>

#include <string>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

struct WOLFSSL;

namespace parties::server {

struct Session {
    uint32_t         id = 0;             // Internal session ID (assigned by TLS server)
    WOLFSSL*         ssl = nullptr;      // TLS connection
    int              socket_fd = -1;     // Underlying TCP socket

    // Authenticated state (set after AUTH_RESPONSE)
    bool             authenticated = false;
    UserId           user_id = 0;
    std::string      username;
    int              role = 3;           // Default: User
    SessionToken     session_token{};
    EnetToken        enet_token{};

    // Voice state
    ChannelId        channel_id = 0;     // 0 = not in a channel
    bool             muted = false;
    bool             deafened = false;

    // Screen share metadata (set when sharing, used for late-join notifications)
    uint8_t          share_codec = 0;
    uint16_t         share_width = 0;
    uint16_t         share_height = 0;

    // Subscribe state: whose video stream this viewer is watching (0 = none)
    UserId           subscribed_sharer = 0;

    // Connection state
    std::atomic<bool> alive{true};

    // Thread-safe send over TLS
    std::mutex       write_mutex;
    bool send(const uint8_t* data, size_t len);
    bool send_message(protocol::ControlMessageType type, const uint8_t* payload, size_t payload_len);
};

} // namespace parties::server
