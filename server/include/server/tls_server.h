#pragma once

#include <server/session.h>
#include <parties/thread_queue.h>
#include <parties/protocol.h>

#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <unordered_map>

struct WOLFSSL_CTX;

namespace parties::server {

// A control message received from a client
struct IncomingMessage {
    uint32_t                      session_id;
    protocol::ControlMessageType  type;
    std::vector<uint8_t>          payload;
};

class TlsServer {
public:
    TlsServer();
    ~TlsServer();

    // Start listening. Returns false on failure.
    bool start(const std::string& listen_ip, uint16_t port,
               const std::string& cert_file, const std::string& key_file);

    // Stop the server, close all connections
    void stop();

    // Queue of messages received from clients
    ThreadQueue<IncomingMessage>& incoming() { return incoming_; }

    // Send a message to a specific session
    bool send_to(uint32_t session_id, protocol::ControlMessageType type,
                 const uint8_t* payload, size_t payload_len);

    // Broadcast to all authenticated sessions
    void broadcast(protocol::ControlMessageType type,
                   const uint8_t* payload, size_t payload_len);

    // Disconnect a session
    void disconnect(uint32_t session_id);

    // Get a session (may return nullptr)
    std::shared_ptr<Session> get_session(uint32_t session_id);

    // Get all sessions (snapshot)
    std::vector<std::shared_ptr<Session>> get_sessions();

    // Callback for when a session disconnects
    std::function<void(uint32_t session_id)> on_disconnect;

private:
    void accept_loop();
    void client_reader(std::shared_ptr<Session> session);
    void remove_session(uint32_t session_id);

    WOLFSSL_CTX* ctx_ = nullptr;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::mutex sessions_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions_;
    std::unordered_map<uint32_t, std::thread> reader_threads_;
    uint32_t next_session_id_ = 1;

    ThreadQueue<IncomingMessage> incoming_;
};

} // namespace parties::server
