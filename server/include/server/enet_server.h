#pragma once

#include <parties/types.h>
#include <parties/thread_queue.h>

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <vector>

#include <enet.h>

namespace parties::server {

struct DataPacket {
    uint32_t    session_id;    // Mapped from ENet peer -> session via enet_token
    uint8_t     packet_type;   // VOICE_PACKET_TYPE etc.
    uint8_t     channel_id;    // ENet channel the packet arrived on
    bool        reliable;      // Was it sent reliably?
    std::vector<uint8_t> data;
};

class EnetServer {
public:
    EnetServer();
    ~EnetServer();

    // Start the ENet host on the given address/port
    bool start(const std::string& listen_ip, uint16_t port, size_t max_clients);

    // Stop the server
    void stop();

    // Must be called periodically (or runs in its own thread)
    // Services ENet events for up to timeout_ms milliseconds
    void service(uint32_t timeout_ms = 5);

    // Start servicing in a background thread
    void start_service_thread();

    // Register an enet_token -> session_id mapping (called after auth)
    void bind_token(const EnetToken& token, uint32_t session_id);

    // Remove a session binding
    void unbind_session(uint32_t session_id);

    // Send a data packet to a specific peer by session_id
    bool send_to(uint32_t session_id, const uint8_t* data, size_t len,
                 bool reliable = false);

    // Send on a specific ENet channel with explicit flags
    bool send_to_on_channel(uint32_t session_id, uint8_t channel,
                            const uint8_t* data, size_t len, enet_uint32 flags);

    // Send to all peers in a list of session IDs (voice fan-out)
    void send_to_many(const std::vector<uint32_t>& session_ids,
                      const uint8_t* data, size_t len);

    // Send to many on a specific channel with explicit flags
    void send_to_many_on_channel(const std::vector<uint32_t>& session_ids,
                                 uint8_t channel, const uint8_t* data, size_t len,
                                 enet_uint32 flags);

    // Queue of received data packets
    ThreadQueue<DataPacket>& incoming() { return incoming_; }

    // Callback when a peer disconnects
    std::function<void(uint32_t session_id)> on_disconnect;

private:
    void service_loop();

    ENetHost* host_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread service_thread_;

    // Token -> session_id mapping for authenticating ENet connections
    std::mutex token_mutex_;
    std::unordered_map<std::string, uint32_t> token_to_session_;

    // session_id -> ENetPeer* mapping
    std::mutex peers_mutex_;
    std::unordered_map<uint32_t, ENetPeer*> session_peers_;

    ThreadQueue<DataPacket> incoming_;

    // Helper: convert EnetToken to string key
    static std::string token_key(const EnetToken& token);
};

} // namespace parties::server
