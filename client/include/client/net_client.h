#pragma once

#include <parties/types.h>
#include <parties/protocol.h>
#include <parties/thread_queue.h>

#include <string>
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

struct WOLFSSL_CTX;
struct WOLFSSL;

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace parties::client {

// A control message received from the server
struct ServerMessage {
    protocol::ControlMessageType type;
    std::vector<uint8_t>         payload;
};

class NetClient {
public:
    NetClient();
    ~NetClient();

    // Connect to server's TLS control plane
    bool connect(const std::string& host, uint16_t port);

    // Get the server certificate fingerprint (after TLS connect)
    std::string get_server_fingerprint() const;

    // Disconnect from server
    void disconnect();

    bool is_connected() const { return connected_; }

    // Send a control message to the server
    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len);

    // Connect ENet data plane using the enet_token from auth
    bool connect_data(const std::string& host, uint16_t port,
                      const EnetToken& token);

    // Disconnect ENet
    void disconnect_data();

    // Service ENet (call periodically)
    void service_enet(uint32_t timeout_ms = 5);

    // Send a data packet on the voice channel
    bool send_data(const uint8_t* data, size_t len, bool reliable = false);

    // Send a video packet on the video channel
    bool send_video(const uint8_t* data, size_t len, bool reliable);

    // Queue of messages from the server (TLS control plane)
    ThreadQueue<ServerMessage>& incoming() { return incoming_; }

    // Callback for data packets received from ENet
    std::function<void(const uint8_t* data, size_t len)> on_data_received;

    // Callback for disconnect events
    std::function<void()> on_disconnected;

private:
    void reader_loop();

    // TLS control plane
    WOLFSSL_CTX* tls_ctx_ = nullptr;
    WOLFSSL*     ssl_ = nullptr;
    int          socket_fd_ = -1;
    std::atomic<bool> connected_{false};
    std::thread  reader_thread_;
    std::mutex   write_mutex_;

    // ENet data plane (all access guarded by enet_mutex_)
    ENetHost*    enet_host_ = nullptr;
    ENetPeer*    enet_peer_ = nullptr;
    std::atomic<bool> data_connected_{false};
    std::mutex   enet_mutex_;

    ThreadQueue<ServerMessage> incoming_;
    std::string server_fingerprint_;
};

} // namespace parties::client
