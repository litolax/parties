#include <server/tls_server.h>
#include <parties/crypto.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socklen_t = int;
   static int close_socket(int fd) { return closesocket(fd); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   static int close_socket(int fd) { return close(fd); }
#endif

#include <cstdio>
#include <cstring>

namespace parties::server {

TlsServer::TlsServer() = default;

TlsServer::~TlsServer() {
    stop();
}

bool TlsServer::start(const std::string& listen_ip, uint16_t port,
                       const std::string& cert_file, const std::string& key_file) {
    ctx_ = parties::create_tls_server_ctx(cert_file, key_file);
    if (!ctx_) {
        std::fprintf(stderr, "[TLS] Failed to create TLS context\n");
        return false;
    }

    // Create TCP socket
    listen_fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "[TLS] Failed to create socket\n");
        parties::free_tls_ctx(ctx_);
        ctx_ = nullptr;
        return false;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, listen_ip.c_str(), &addr.sin_addr);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[TLS] Failed to bind to %s:%u\n", listen_ip.c_str(), port);
        close_socket(listen_fd_);
        parties::free_tls_ctx(ctx_);
        ctx_ = nullptr;
        return false;
    }

    if (listen(listen_fd_, 16) < 0) {
        std::fprintf(stderr, "[TLS] Failed to listen\n");
        close_socket(listen_fd_);
        parties::free_tls_ctx(ctx_);
        ctx_ = nullptr;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&TlsServer::accept_loop, this);

    std::printf("[TLS] Listening on %s:%u\n", listen_ip.c_str(), port);
    return true;
}

void TlsServer::stop() {
    if (!running_) return;
    running_ = false;

    // Close listen socket to unblock accept()
    if (listen_fd_ >= 0) {
        close_socket(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable())
        accept_thread_.join();

    // Close all sessions
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
        session->alive = false;
        if (session->ssl) {
            wolfSSL_shutdown(session->ssl);
            wolfSSL_free(session->ssl);
            session->ssl = nullptr;
        }
        if (session->socket_fd >= 0) {
            close_socket(session->socket_fd);
            session->socket_fd = -1;
        }
    }
    for (auto& [id, thread] : reader_threads_) {
        if (thread.joinable())
            thread.join();
    }
    sessions_.clear();
    reader_threads_.clear();

    if (ctx_) {
        parties::free_tls_ctx(ctx_);
        ctx_ = nullptr;
    }
}

void TlsServer::accept_loop() {
    while (running_) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = static_cast<int>(accept(listen_fd_,
                        reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len));
        if (client_fd < 0) {
            if (!running_) break;
            continue;
        }

        // Create wolfSSL session
        WOLFSSL* ssl = wolfSSL_new(ctx_);
        if (!ssl) {
            std::fprintf(stderr, "[TLS] Failed to create SSL object\n");
            close_socket(client_fd);
            continue;
        }

        wolfSSL_set_fd(ssl, client_fd);

        // TLS handshake
        if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
            int err = wolfSSL_get_error(ssl, 0);
            char buf[256];
            wolfSSL_ERR_error_string(err, buf);
            std::fprintf(stderr, "[TLS] Handshake failed: %s\n", buf);
            wolfSSL_free(ssl);
            close_socket(client_fd);
            continue;
        }

        // Create session
        auto session = std::make_shared<Session>();
        session->ssl = ssl;
        session->socket_fd = client_fd;

        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            session->id = next_session_id_++;
            sessions_[session->id] = session;
            reader_threads_[session->id] = std::thread(&TlsServer::client_reader, this, session);
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::printf("[TLS] Client connected: session %u from %s:%u\n",
                    session->id, ip_str, ntohs(client_addr.sin_port));
    }
}

void TlsServer::client_reader(std::shared_ptr<Session> session) {
    // Read length-prefixed messages: [u32 length][u16 type][payload]
    while (session->alive && running_) {
        // Read message length (4 bytes, little-endian)
        uint8_t len_buf[4];
        int total = 0;
        while (total < 4) {
            int r = wolfSSL_read(session->ssl, len_buf + total, 4 - total);
            if (r <= 0) {
                session->alive = false;
                break;
            }
            total += r;
        }
        if (!session->alive) break;

        uint32_t msg_len;
        std::memcpy(&msg_len, len_buf, 4);

        // Sanity check: max 1MB message
        if (msg_len < 2 || msg_len > 1024 * 1024) {
            std::fprintf(stderr, "[TLS] Session %u: invalid message length %u\n",
                         session->id, msg_len);
            session->alive = false;
            break;
        }

        // Read message type (2 bytes)
        uint8_t type_buf[2];
        total = 0;
        while (total < 2) {
            int r = wolfSSL_read(session->ssl, type_buf + total, 2 - total);
            if (r <= 0) { session->alive = false; break; }
            total += r;
        }
        if (!session->alive) break;

        uint16_t raw_type;
        std::memcpy(&raw_type, type_buf, 2);

        // Read payload
        uint32_t payload_len = msg_len - 2;
        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0) {
            total = 0;
            while (static_cast<uint32_t>(total) < payload_len) {
                int r = wolfSSL_read(session->ssl, payload.data() + total,
                                     static_cast<int>(payload_len - total));
                if (r <= 0) { session->alive = false; break; }
                total += r;
            }
            if (!session->alive) break;
        }

        IncomingMessage msg;
        msg.session_id = session->id;
        msg.type = static_cast<protocol::ControlMessageType>(raw_type);
        msg.payload = std::move(payload);
        incoming_.push(std::move(msg));
    }

    std::printf("[TLS] Session %u disconnected\n", session->id);

    // Notify disconnect callback
    uint32_t sid = session->id;
    if (on_disconnect)
        on_disconnect(sid);

    remove_session(sid);
}

void TlsServer::remove_session(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        auto& session = it->second;
        if (session->ssl) {
            wolfSSL_shutdown(session->ssl);
            wolfSSL_free(session->ssl);
            session->ssl = nullptr;
        }
        if (session->socket_fd >= 0) {
            close_socket(session->socket_fd);
            session->socket_fd = -1;
        }
        sessions_.erase(it);
    }
    // Detach the reader thread so it can finish
    auto rt = reader_threads_.find(session_id);
    if (rt != reader_threads_.end()) {
        if (rt->second.joinable())
            rt->second.detach();
        reader_threads_.erase(rt);
    }
}

bool TlsServer::send_to(uint32_t session_id, protocol::ControlMessageType type,
                         const uint8_t* payload, size_t payload_len) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        session = it->second;
    }
    return session->send_message(type, payload, payload_len);
}

void TlsServer::broadcast(protocol::ControlMessageType type,
                           const uint8_t* payload, size_t payload_len) {
    auto sessions = get_sessions();
    for (auto& s : sessions) {
        if (s->authenticated)
            s->send_message(type, payload, payload_len);
    }
}

void TlsServer::disconnect(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end())
        it->second->alive = false;
}

std::shared_ptr<Session> TlsServer::get_session(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return nullptr;
    return it->second;
}

std::vector<std::shared_ptr<Session>> TlsServer::get_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::vector<std::shared_ptr<Session>> result;
    result.reserve(sessions_.size());
    for (auto& [id, s] : sessions_)
        result.push_back(s);
    return result;
}

} // namespace parties::server
