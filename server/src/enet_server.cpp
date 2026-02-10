#include <server/enet_server.h>
#include <parties/protocol.h>

#include <cstdio>
#include <cstring>

namespace parties::server {

EnetServer::EnetServer() = default;

EnetServer::~EnetServer() {
    stop();
}

std::string EnetServer::token_key(const EnetToken& token) {
    return std::string(reinterpret_cast<const char*>(token.data()), token.size());
}

bool EnetServer::start(const std::string& listen_ip, uint16_t port, size_t max_clients) {
    ENetAddress address = {};
    //enet_address_set_host_ip(&address, listen_ip.c_str());
    address.host = ENET_HOST_ANY;
    address.port = port;

    // 3 channels: 0 = auth/control, 1 = voice, 2 = video
    host_ = enet_host_create(&address, max_clients, protocol::ENET_NUM_CHANNELS, 0, 0);
    if (!host_) {
        std::fprintf(stderr, "[ENet] Failed to create host on %s:%u\n",
                     listen_ip.c_str(), port);
        return false;
    }

    running_ = true;
    std::printf("[ENet] Listening on %s:%u (max %zu clients)\n",
                listen_ip.c_str(), port, max_clients);
    return true;
}

void EnetServer::stop() {
    if (!running_) return;
    running_ = false;

    if (service_thread_.joinable())
        service_thread_.join();

    if (host_) {
        // Disconnect all peers
        for (size_t i = 0; i < host_->peerCount; i++) {
            if (host_->peers[i].state == ENET_PEER_STATE_CONNECTED)
                enet_peer_disconnect_now(&host_->peers[i], 0);
        }
        enet_host_destroy(host_);
        host_ = nullptr;
    }
}

void EnetServer::service(uint32_t timeout_ms) {
    if (!host_) return;

    ENetEvent event;
    while (enet_host_service(host_, &event, timeout_ms) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            char ip[64];
            enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
            std::printf("[ENet] Peer connected from %s:%u\n",
                        ip, event.peer->address.port);
            // Peer must send enet_token to authenticate
            enet_peer_set_data(event.peer, nullptr);
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE: {
            // Check if peer is authenticated
            void* peer_data = enet_peer_get_data(event.peer);

            if (!peer_data) {
                // First packet should be the 32-byte enet_token on channel 0
                if (event.channelID == 0 && event.packet->dataLength == 32) {
                    EnetToken token;
                    std::memcpy(token.data(), event.packet->data, 32);
                    std::string key = token_key(token);

                    uint32_t session_id = 0;
                    {
                        std::lock_guard<std::mutex> lock(token_mutex_);
                        auto it = token_to_session_.find(key);
                        if (it != token_to_session_.end()) {
                            session_id = it->second;
                            token_to_session_.erase(it);
                        }
                    }

                    if (session_id != 0) {
                        // Store session_id in peer data (as uintptr_t)
                        enet_peer_set_data(event.peer,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(session_id)));
                        {
                            std::lock_guard<std::mutex> lock(peers_mutex_);
                            session_peers_[session_id] = event.peer;
                        }
                        std::printf("[ENet] Peer authenticated as session %u\n", session_id);
                    } else {
                        std::fprintf(stderr, "[ENet] Invalid enet_token, disconnecting peer\n");
                        enet_peer_disconnect(event.peer, 0);
                    }
                } else {
                    std::fprintf(stderr, "[ENet] Unauthenticated peer sent unexpected data\n");
                    enet_peer_disconnect(event.peer, 0);
                }
            } else {
                // Authenticated peer — route data packet
                uint32_t session_id = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(peer_data));

                if (event.packet->dataLength > 0) {
                    DataPacket pkt;
                    pkt.session_id = session_id;
                    pkt.packet_type = event.packet->data[0];
                    pkt.channel_id = event.channelID;
                    pkt.reliable = (event.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0;
                    pkt.data.assign(event.packet->data + 1,
                                    event.packet->data + event.packet->dataLength);
                    incoming_.push(std::move(pkt));
                }
            }
            enet_packet_destroy(event.packet);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT:
        case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
            void* peer_data = enet_peer_get_data(event.peer);
            if (peer_data) {
                uint32_t session_id = static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(peer_data));
                {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    session_peers_.erase(session_id);
                }
                std::printf("[ENet] Session %u disconnected\n", session_id);
                if (on_disconnect)
                    on_disconnect(session_id);
            }
            enet_peer_set_data(event.peer, nullptr);
            break;
        }

        default:
            break;
        }
        // After first event, don't block on subsequent ones
        timeout_ms = 0;
    }
}

void EnetServer::start_service_thread() {
    service_thread_ = std::thread(&EnetServer::service_loop, this);
}

void EnetServer::service_loop() {
    while (running_) {
        service(10);
    }
}

void EnetServer::bind_token(const EnetToken& token, uint32_t session_id) {
    std::lock_guard<std::mutex> lock(token_mutex_);
    token_to_session_[token_key(token)] = session_id;
}

void EnetServer::unbind_session(uint32_t session_id) {
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = session_peers_.find(session_id);
        if (it != session_peers_.end()) {
            enet_peer_disconnect(it->second, 0);
            session_peers_.erase(it);
        }
    }
}

bool EnetServer::send_to(uint32_t session_id, const uint8_t* data, size_t len,
                          bool reliable) {
    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE
                                 : ENET_PACKET_FLAG_UNSEQUENCED;
    return send_to_on_channel(session_id, protocol::ENET_CHANNEL_VOICE, data, len, flags);
}

bool EnetServer::send_to_on_channel(uint32_t session_id, uint8_t channel,
                                     const uint8_t* data, size_t len,
                                     enet_uint32 flags) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = session_peers_.find(session_id);
    if (it == session_peers_.end()) return false;

    ENetPacket* packet = enet_packet_create(data, len, flags);
    if (!packet) return false;

    return enet_peer_send(it->second, channel, packet) == 0;
}

void EnetServer::send_to_many(const std::vector<uint32_t>& session_ids,
                               const uint8_t* data, size_t len) {
    send_to_many_on_channel(session_ids, protocol::ENET_CHANNEL_VOICE,
                            data, len, ENET_PACKET_FLAG_UNSEQUENCED);
}

void EnetServer::send_to_many_on_channel(const std::vector<uint32_t>& session_ids,
                                          uint8_t channel, const uint8_t* data,
                                          size_t len, enet_uint32 flags) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (uint32_t sid : session_ids) {
        auto it = session_peers_.find(sid);
        if (it == session_peers_.end()) continue;

        ENetPacket* packet = enet_packet_create(data, len, flags);
        if (packet)
            enet_peer_send(it->second, channel, packet);
    }
}

} // namespace parties::server
