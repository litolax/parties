#include <server/session.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <cstring>

namespace parties::server {

bool Session::send(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(write_mutex);
    if (!alive || !ssl) return false;

    size_t total = 0;
    while (total < len) {
        int written = wolfSSL_write(ssl, data + total, static_cast<int>(len - total));
        if (written <= 0) {
            alive = false;
            return false;
        }
        total += static_cast<size_t>(written);
    }
    return true;
}

bool Session::send_message(protocol::ControlMessageType type,
                           const uint8_t* payload, size_t payload_len) {
    // Wire format: [u32 length][u16 type][payload]
    // length = sizeof(u16 type) + payload_len
    uint32_t msg_len = static_cast<uint32_t>(2 + payload_len);
    uint16_t msg_type = static_cast<uint16_t>(type);

    uint8_t header[6];
    std::memcpy(header, &msg_len, 4);
    std::memcpy(header + 4, &msg_type, 2);

    std::lock_guard<std::mutex> lock(write_mutex);
    if (!alive || !ssl) return false;

    // Send header
    int written = wolfSSL_write(ssl, header, 6);
    if (written != 6) { alive = false; return false; }

    // Send payload
    if (payload_len > 0) {
        size_t total = 0;
        while (total < payload_len) {
            written = wolfSSL_write(ssl, payload + total,
                                    static_cast<int>(payload_len - total));
            if (written <= 0) { alive = false; return false; }
            total += static_cast<size_t>(written);
        }
    }
    return true;
}

} // namespace parties::server
