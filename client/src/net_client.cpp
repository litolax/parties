#include <client/net_client.h>
#include <parties/crypto.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <enet.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socklen_t = int;
   static int close_socket(int fd) { return closesocket(fd); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
   static int close_socket(int fd) { return close(fd); }
#endif

#include <cstdio>
#include <cstring>

namespace parties::client {

NetClient::NetClient() = default;

NetClient::~NetClient() {
    disconnect();
    disconnect_data();
}

bool NetClient::connect(const std::string& host, uint16_t port) {
    if (connected_) return false;

    tls_ctx_ = parties::create_tls_client_ctx();
    if (!tls_ctx_) {
        std::fprintf(stderr, "[NetClient] Failed to create TLS context\n");
        return false;
    }

    // Resolve hostname
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Try DNS resolution
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
            std::fprintf(stderr, "[NetClient] Failed to resolve %s\n", host.c_str());
            parties::free_tls_ctx(tls_ctx_);
            tls_ctx_ = nullptr;
            return false;
        }
        addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr)->sin_addr;
        freeaddrinfo(result);
    }

    // Create TCP socket
    socket_fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (socket_fd_ < 0) {
        std::fprintf(stderr, "[NetClient] Failed to create socket\n");
        parties::free_tls_ctx(tls_ctx_);
        tls_ctx_ = nullptr;
        return false;
    }

    if (::connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[NetClient] Failed to connect to %s:%u\n",
                     host.c_str(), port);
        close_socket(socket_fd_);
        socket_fd_ = -1;
        parties::free_tls_ctx(tls_ctx_);
        tls_ctx_ = nullptr;
        return false;
    }

    // TLS handshake
    ssl_ = wolfSSL_new(tls_ctx_);
    if (!ssl_) {
        std::fprintf(stderr, "[NetClient] Failed to create SSL object\n");
        close_socket(socket_fd_);
        socket_fd_ = -1;
        parties::free_tls_ctx(tls_ctx_);
        tls_ctx_ = nullptr;
        return false;
    }

    wolfSSL_set_fd(ssl_, socket_fd_);

    if (wolfSSL_connect(ssl_) != SSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl_, 0);
        char buf[256];
        wolfSSL_ERR_error_string(err, buf);
        std::fprintf(stderr, "[NetClient] TLS handshake failed: %s\n", buf);
        wolfSSL_free(ssl_);
        ssl_ = nullptr;
        close_socket(socket_fd_);
        socket_fd_ = -1;
        parties::free_tls_ctx(tls_ctx_);
        tls_ctx_ = nullptr;
        return false;
    }

    // Get server certificate fingerprint (for TOFU)
    server_fingerprint_ = parties::get_peer_cert_fingerprint(ssl_);
    std::printf("[NetClient] Connected to %s:%u (fingerprint: %s)\n",
                host.c_str(), port, server_fingerprint_.c_str());

    connected_ = true;
    reader_thread_ = std::thread(&NetClient::reader_loop, this);

    return true;
}

std::string NetClient::get_server_fingerprint() const {
    return server_fingerprint_;
}

void NetClient::disconnect() {
    if (!connected_) return;
    connected_ = false;

    if (ssl_) {
        wolfSSL_shutdown(ssl_);
        wolfSSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (socket_fd_ >= 0) {
        close_socket(socket_fd_);
        socket_fd_ = -1;
    }
    if (reader_thread_.joinable())
        reader_thread_.join();

    if (tls_ctx_) {
        parties::free_tls_ctx(tls_ctx_);
        tls_ctx_ = nullptr;
    }
}

bool NetClient::send_message(protocol::ControlMessageType type,
                             const uint8_t* payload, size_t payload_len) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!connected_ || !ssl_) return false;

    // Wire format: [u32 length][u16 type][payload]
    uint32_t msg_len = static_cast<uint32_t>(2 + payload_len);
    uint16_t msg_type = static_cast<uint16_t>(type);

    uint8_t header[6];
    std::memcpy(header, &msg_len, 4);
    std::memcpy(header + 4, &msg_type, 2);

    int written = wolfSSL_write(ssl_, header, 6);
    if (written != 6) { connected_ = false; return false; }

    if (payload_len > 0) {
        size_t total = 0;
        while (total < payload_len) {
            written = wolfSSL_write(ssl_, payload + total,
                                    static_cast<int>(payload_len - total));
            if (written <= 0) { connected_ = false; return false; }
            total += static_cast<size_t>(written);
        }
    }
    return true;
}

void NetClient::reader_loop() {
    while (connected_) {
        // Read message length (4 bytes)
        uint8_t len_buf[4];
        int total = 0;
        while (total < 4) {
            int r = wolfSSL_read(ssl_, len_buf + total, 4 - total);
            if (r <= 0) { connected_ = false; break; }
            total += r;
        }
        if (!connected_) break;

        uint32_t msg_len;
        std::memcpy(&msg_len, len_buf, 4);

        if (msg_len < 2 || msg_len > 1024 * 1024) {
            std::fprintf(stderr, "[NetClient] Invalid message length %u\n", msg_len);
            connected_ = false;
            break;
        }

        // Read message type (2 bytes)
        uint8_t type_buf[2];
        total = 0;
        while (total < 2) {
            int r = wolfSSL_read(ssl_, type_buf + total, 2 - total);
            if (r <= 0) { connected_ = false; break; }
            total += r;
        }
        if (!connected_) break;

        uint16_t raw_type;
        std::memcpy(&raw_type, type_buf, 2);

        // Read payload
        uint32_t payload_len = msg_len - 2;
        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0) {
            total = 0;
            while (static_cast<uint32_t>(total) < payload_len) {
                int r = wolfSSL_read(ssl_, payload.data() + total,
                                     static_cast<int>(payload_len - total));
                if (r <= 0) { connected_ = false; break; }
                total += r;
            }
            if (!connected_) break;
        }

        ServerMessage msg;
        msg.type = static_cast<protocol::ControlMessageType>(raw_type);
        msg.payload = std::move(payload);
        incoming_.push(std::move(msg));
    }

    std::printf("[NetClient] Disconnected from server\n");
    if (on_disconnected)
        on_disconnected();
}

bool NetClient::connect_data(const std::string& host, uint16_t port,
                              const EnetToken& token) {
    if (data_connected_) return false;

    // Create ENet client host (no incoming connections, 3 channels)
    enet_host_ = enet_host_create(nullptr, 1, protocol::ENET_NUM_CHANNELS, 0, 0);
    if (!enet_host_) {
        std::fprintf(stderr, "[NetClient] Failed to create ENet host\n");
        return false;
    }

    ENetAddress address = {};
    int resolve_result = enet_address_set_host(&address, host.c_str());
    address.port = port;

    if (resolve_result < 0) {
        std::fprintf(stderr, "[NetClient] enet_address_set_host failed for '%s'\n",
                     host.c_str());
        enet_host_destroy(enet_host_);
        enet_host_ = nullptr;
        return false;
    }

    // Log the resolved address
    char resolved_ip[64] = {};
    enet_address_get_host_ip(&address, resolved_ip, sizeof(resolved_ip));
    std::printf("[NetClient] ENet connecting to %s:%u (resolved: %s)\n",
                host.c_str(), port, resolved_ip);

    // Connect
    enet_peer_ = enet_host_connect(enet_host_, &address, protocol::ENET_NUM_CHANNELS, 0);
    if (!enet_peer_) {
        std::fprintf(stderr, "[NetClient] Failed to initiate ENet connection\n");
        enet_host_destroy(enet_host_);
        enet_host_ = nullptr;
        return false;
    }

    // Wait for connection (up to 5 seconds)
    ENetEvent event;
    int service_result = enet_host_service(enet_host_, &event, 5000);
    if (service_result > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
        std::printf("[NetClient] ENet connected to %s:%u\n", host.c_str(), port);
    } else {
        std::fprintf(stderr, "[NetClient] ENet connection failed: service=%d event=%d\n",
                     service_result, service_result > 0 ? event.type : -1);
        enet_peer_reset(enet_peer_);
        enet_host_destroy(enet_host_);
        enet_host_ = nullptr;
        enet_peer_ = nullptr;
        return false;
    }

    // Send enet_token as first packet (reliable, channel 0)
    ENetPacket* pkt = enet_packet_create(token.data(), token.size(),
                                         ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(enet_peer_, 0, pkt) < 0) {
        std::fprintf(stderr, "[NetClient] Failed to send enet_token\n");
        enet_peer_disconnect(enet_peer_, 0);
        enet_host_destroy(enet_host_);
        enet_host_ = nullptr;
        enet_peer_ = nullptr;
        return false;
    }
    enet_host_flush(enet_host_);

    data_connected_ = true;
    return true;
}

void NetClient::disconnect_data() {
    if (!data_connected_) return;
    data_connected_ = false;

    if (enet_peer_) {
        enet_peer_disconnect(enet_peer_, 0);
        // Allow time for disconnect to complete
        ENetEvent event;
        while (enet_host_service(enet_host_, &event, 500) > 0) {
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
        }
        enet_peer_ = nullptr;
    }

    if (enet_host_) {
        enet_host_destroy(enet_host_);
        enet_host_ = nullptr;
    }
}

void NetClient::service_enet(uint32_t timeout_ms) {
    if (!enet_host_ || !data_connected_) return;

    std::lock_guard<std::mutex> lock(enet_mutex_);
    ENetEvent event;
    while (enet_host_service(enet_host_, &event, timeout_ms) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE:
            if (on_data_received && event.packet->dataLength > 0)
                on_data_received(event.packet->data, event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
        case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
            data_connected_ = false;
            enet_peer_ = nullptr;
            std::printf("[NetClient] ENet disconnected\n");
            break;

        default:
            break;
        }
        timeout_ms = 0;
    }
}

bool NetClient::send_data(const uint8_t* data, size_t len, bool reliable) {
    if (!data_connected_ || !enet_peer_) return false;

    std::lock_guard<std::mutex> lock(enet_mutex_);
    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE
                                 : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, len, flags);
    if (!packet) return false;

    return enet_peer_send(enet_peer_, protocol::ENET_CHANNEL_VOICE, packet) == 0;
}

bool NetClient::send_video(const uint8_t* data, size_t len, bool reliable) {
    if (!data_connected_ || !enet_peer_) return false;

    std::lock_guard<std::mutex> lock(enet_mutex_);
    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE
                                 : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
    ENetPacket* packet = enet_packet_create(data, len, flags);
    if (!packet) return false;

    return enet_peer_send(enet_peer_, protocol::ENET_CHANNEL_VIDEO, packet) == 0;
}

} // namespace parties::client
