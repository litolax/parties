// Apple QUIC transport for macOS (ARM) and iOS.
//
// Implements NetClient::Impl using Network.framework (NWConnection/NWParameters).
// Compiled instead of net_client_msquic.cpp on Apple platforms.
//
// Three multiplexed QUIC streams on the same underlying connection:
//   connection  — bidirectional stream 0: control messages (AUTH, channels, …)
//   video_conn  — bidirectional stream 4: video frames (screen share send/recv)
//   dgram_conn  — QUIC datagram flow:    voice packets (unreliable)
//
// Network.framework coalesces connections to the same endpoint with the same
// ALPN onto one QUIC session, so all three share one TLS handshake.
//
// Requires macOS 12+ / iOS 15+ (QUIC); datagram flow requires macOS 14+ / iOS 17+.

#include <client/net_client.h>
#include <parties/profiler.h>
#include <parties/protocol.h>
#include <parties/crypto.h>

#include "net_client_parsing.h"

#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace parties::client {

// ── Helpers ───────────────────────────────────────────────────────────────────

static void log_nw_error(const char* prefix, nw_error_t error)
{
    if (!error) { std::fprintf(stderr, "%s\n", prefix); return; }
    nw_error_domain_t domain = nw_error_get_error_domain(error);
    int               code   = nw_error_get_error_code(error);
    const char*       dname  = "unknown";
    switch (domain) {
        case nw_error_domain_posix:  dname = "posix";  break;
        case nw_error_domain_dns:    dname = "dns";    break;
        case nw_error_domain_tls:    dname = "tls";    break;
        default: break;
    }
    CFErrorRef cf = nw_error_copy_cf_error(error);
    if (cf) {
        CFStringRef desc = CFErrorCopyDescription(cf);
        char        buf[512] = {};
        CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
        std::fprintf(stderr, "%s: [%s %d] %s\n", prefix, dname, code, buf);
        CFRelease(desc);
        CFRelease(cf);
    } else {
        std::fprintf(stderr, "%s: [%s %d]\n", prefix, dname, code);
    }
}

// Create QUIC parameters with ALPN "parties" and self-signed cert acceptance.
// is_datagram=true → datagram flow (QUIC datagram extension frames).
static nw_parameters_t make_quic_params(bool is_datagram)
{
    nw_parameters_t params = nw_parameters_create_quic(
        ^(nw_protocol_options_t quic_options) {
            nw_quic_add_tls_application_protocol(quic_options, "parties");
            if (is_datagram)
                nw_quic_set_stream_is_datagram(quic_options, true);
        });

    // Accept self-signed certificates — TOFU handled at app level.
    nw_protocol_stack_t   stack = nw_parameters_copy_default_protocol_stack(params);
    nw_protocol_options_t quic  = nw_protocol_stack_copy_transport_protocol(stack);
    if (quic) {
        sec_protocol_options_t sec = nw_quic_copy_sec_protocol_options(quic);
        if (sec) {
            sec_protocol_options_set_verify_block(sec,
                ^(sec_protocol_metadata_t /*m*/, sec_trust_t /*t*/,
                  sec_protocol_verify_complete_t done) { done(true); },
                dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
            nw_release(sec);
        }
        nw_release(quic);
    }
    nw_release(stack);
    return params;
}

// ── Impl ──────────────────────────────────────────────────────────────────────

struct NetClient::Impl {
    NetClient& parent;

    nw_connection_t  connection  = nullptr;  // stream 0: control (opened on connect)
    nw_connection_t  video_conn  = nullptr;  // stream 4: video  (opened after auth)
    nw_connection_t  dgram_conn  = nullptr;  // datagram flow    (opened after auth)
    dispatch_queue_t queue       = nullptr;

    // Stored so open_av_streams() can reach the same endpoint.
    std::string saved_host;
    uint16_t    saved_port = 0;

    std::atomic<bool> connected{false};
    std::atomic<bool> connecting{false};
    std::atomic<bool> connect_failed_{false};
    std::mutex        write_mutex;

    std::mutex           buffer_mutex;
    std::vector<uint8_t> recv_buffer;         // control stream accumulator

    std::mutex           video_buffer_mutex;
    std::vector<uint8_t> video_recv_buffer;   // video stream accumulator

    std::string server_fingerprint;

    explicit Impl(NetClient& p) : parent(p) {}
    ~Impl() { disconnect(); }

    // ── connect ───────────────────────────────────────────────────────────

    bool connect(const std::string& host, uint16_t port,
                 const uint8_t* /*ticket*/, size_t /*ticket_len*/)
    {
        if (connected || connecting) return false;

        saved_host = host;
        saved_port = port;

        nw_endpoint_t endpoint =
            nw_endpoint_create_host(host.c_str(), std::to_string(port).c_str());

        queue = dispatch_queue_create("com.parties.netclient", DISPATCH_QUEUE_SERIAL);

        __block Impl* self = this;

        // Only the control stream is opened here (stream 0).
        // Video (stream 4) and datagram flow are opened after auth via
        // open_av_streams() to guarantee stream ID ordering.
        {
            nw_parameters_t p = make_quic_params(false);
            connection = nw_connection_create(endpoint, p);
            nw_release(p);
        }
        nw_release(endpoint);

        nw_connection_set_queue(connection, queue);
        nw_connection_set_state_changed_handler(connection,
            ^(nw_connection_state_t state, nw_error_t error) {
                self->on_control_state_changed(state, error);
            });
        nw_connection_start(connection);

        std::printf("[NetClient] Connecting to %s:%u (Network.framework QUIC)...\n",
                    host.c_str(), port);
        connecting      = true;
        connect_failed_ = false;
        return true;
    }

    // ── disconnect ────────────────────────────────────────────────────────

    void disconnect()
    {
        if (!connection && !video_conn && !dgram_conn) return;

        connected       = false;
        connecting      = false;
        connect_failed_ = false;

        auto cancel = [](nw_connection_t& c) {
            if (c) { nw_connection_cancel(c); nw_release(c); c = nullptr; }
        };
        cancel(dgram_conn);
        cancel(video_conn);
        cancel(connection);

        if (queue) {
            dispatch_release(queue);
            queue = nullptr;
        }

        { std::lock_guard lock(buffer_mutex);       recv_buffer.clear(); }
        { std::lock_guard lock(video_buffer_mutex); video_recv_buffer.clear(); }
        server_fingerprint.clear();
    }

    // ── send ──────────────────────────────────────────────────────────────

    bool send_message(protocol::ControlMessageType type,
                      const uint8_t* payload, size_t payload_len)
    {
        if (!connected || !connection) return false;

        std::printf("[NetClient] send_message: type=%d payload=%zu bytes\n",
                    (int)type, payload_len);

        // Wire: [u32 msg_len][u16 type][payload]  (msg_len = 2 + payload_len)
        uint32_t msg_len   = static_cast<uint32_t>(2 + payload_len);
        size_t   total_len = 6 + payload_len;

        auto* buf = new uint8_t[total_len];
        std::memcpy(buf, &msg_len, 4);
        uint16_t t = static_cast<uint16_t>(type);
        std::memcpy(buf + 4, &t, 2);
        if (payload_len > 0) std::memcpy(buf + 6, payload, payload_len);

        dispatch_data_t data =
            dispatch_data_create(buf, total_len,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                                 ^{ delete[] buf; });
        nw_connection_send(connection, data,
                           NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false,
                           ^(nw_error_t err) {
                               if (err) std::fprintf(stderr, "[NetClient] send_message error\n");
                           });
        dispatch_release(data);
        return true;
    }

    bool send_data(const uint8_t* data, size_t len, bool /*reliable*/)
    {
        if (!connected || !dgram_conn) return false;

        // Voice: send as QUIC datagram (is_final=true).
        auto* buf = new uint8_t[len];
        std::memcpy(buf, data, len);

        dispatch_data_t dd =
            dispatch_data_create(buf, len,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                                 ^{ delete[] buf; });
        nw_connection_send(dgram_conn, dd,
                           NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT,
                           true,  // is_final=true → QUIC datagram frame
                           ^(nw_error_t err) {
                               if (err) std::fprintf(stderr, "[NetClient] send_data error\n");
                           });
        dispatch_release(dd);
        return true;
    }

    bool send_video(const uint8_t* data, size_t len, bool /*reliable*/)
    {
        if (!connected || !video_conn) return false;

        // Wire: [u32 frame_len][data]
        size_t   total_len = 4 + len;
        auto*    buf       = new uint8_t[total_len];
        uint32_t flen      = static_cast<uint32_t>(len);
        std::memcpy(buf, &flen, 4);
        std::memcpy(buf + 4, data, len);

        dispatch_data_t dd =
            dispatch_data_create(buf, total_len,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                                 ^{ delete[] buf; });
        nw_connection_send(video_conn, dd,
                           NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false,
                           ^(nw_error_t err) {
                               if (err) std::fprintf(stderr, "[NetClient] send_video error\n");
                           });
        dispatch_release(dd);
        return true;
    }

    // ── open_av_streams (called after auth) ──────────────────────────────

    void open_av_streams()
    {
        if (!connected || !queue) return;
        if (video_conn || dgram_conn) return;  // already open

        nw_endpoint_t endpoint =
            nw_endpoint_create_host(saved_host.c_str(),
                                    std::to_string(saved_port).c_str());
        __block Impl* self = this;

        // ── Video stream (stream 4) ────────────────────────────────────────
        {
            nw_parameters_t p = make_quic_params(false);
            video_conn = nw_connection_create(endpoint, p);
            nw_release(p);
        }
        if (video_conn) {
            nw_connection_set_queue(video_conn, queue);
            nw_connection_set_state_changed_handler(video_conn,
                ^(nw_connection_state_t state, nw_error_t error) {
                    if (state == nw_connection_state_ready) {
                        std::printf("[NetClient] Video stream ready\n");
                        self->receive_loop_video();
                    } else if (state == nw_connection_state_failed) {
                        log_nw_error("[NetClient] Video stream failed", error);
                    }
                });
            nw_connection_start(video_conn);
        }

        // ── Datagram flow (voice) ──────────────────────────────────────────
        {
            nw_parameters_t p = make_quic_params(true);
            dgram_conn = nw_connection_create(endpoint, p);
            nw_release(p);
        }
        if (dgram_conn) {
            nw_connection_set_queue(dgram_conn, queue);
            nw_connection_set_state_changed_handler(dgram_conn,
                ^(nw_connection_state_t state, nw_error_t error) {
                    if (state == nw_connection_state_ready) {
                        std::printf("[NetClient] Datagram flow ready\n");
                        self->receive_loop_datagram();
                    } else if (state == nw_connection_state_failed) {
                        log_nw_error("[NetClient] Datagram flow failed", error);
                    }
                });
            nw_connection_start(dgram_conn);
        }

        nw_release(endpoint);
        std::printf("[NetClient] Opening video + datagram streams\n");
    }

    // ── State handlers ────────────────────────────────────────────────────

    void on_control_state_changed(nw_connection_state_t state, nw_error_t error)
    {
        switch (state) {
        case nw_connection_state_ready:
            std::printf("[NetClient] QUIC control stream ready\n");
            connecting = false;
            connected  = true;
            receive_loop_control();
            break;

        case nw_connection_state_failed:
            log_nw_error("[NetClient] QUIC connection failed", error);
            if (connecting) connect_failed_ = true;
            connecting = false;
            connected  = false;
            if (parent.on_disconnected) parent.on_disconnected();
            break;

        case nw_connection_state_cancelled:
            std::printf("[NetClient] QUIC connection cancelled\n");
            connecting = false;
            connected  = false;
            if (parent.on_disconnected) parent.on_disconnected();
            break;

        default:
            break;
        }
    }

    // ── Receive loops ─────────────────────────────────────────────────────

    // Control stream — length-prefixed control messages.
    void receive_loop_control()
    {
        if (!connection) return;
        __block Impl* self = this;

        nw_connection_receive(connection, 1, UINT32_MAX,
            ^(dispatch_data_t content, nw_content_context_t /*ctx*/,
              bool /*is_complete*/, nw_error_t error) {
                if (content) {
                    dispatch_data_apply(content,
                        ^bool(dispatch_data_t /*r*/, size_t /*o*/,
                              const void* buf, size_t size) {
                            std::lock_guard lock(self->buffer_mutex);
                            parsing::process_stream_data(
                                static_cast<const uint8_t*>(buf), size,
                                self->recv_buffer, self->parent.incoming());
                            return true;
                        });
                }
                if (error) {
                    log_nw_error("[NetClient] Control stream error", error);
                    return;
                }
                if (self->connection)
                    self->receive_loop_control();
            });
    }

    // Video stream — length-prefixed video frames.
    void receive_loop_video()
    {
        if (!video_conn) return;
        __block Impl* self = this;

        nw_connection_receive(video_conn, 1, UINT32_MAX,
            ^(dispatch_data_t content, nw_content_context_t /*ctx*/,
              bool /*is_complete*/, nw_error_t error) {
                if (content) {
                    dispatch_data_apply(content,
                        ^bool(dispatch_data_t /*r*/, size_t /*o*/,
                              const void* buf, size_t size) {
                            std::lock_guard lock(self->video_buffer_mutex);
                            parsing::process_video_stream_data(
                                static_cast<const uint8_t*>(buf), size,
                                self->video_recv_buffer,
                                self->parent.on_data_received);
                            return true;
                        });
                }
                if (error) {
                    log_nw_error("[NetClient] Video stream error", error);
                    return;
                }
                if (self->video_conn)
                    self->receive_loop_video();
            });
    }

    // Datagram flow — each datagram is one voice packet; pass directly.
    void receive_loop_datagram()
    {
        if (!dgram_conn) return;
        __block Impl* self = this;

        nw_connection_receive(dgram_conn, 1, UINT32_MAX,
            ^(dispatch_data_t content, nw_content_context_t /*ctx*/,
              bool /*is_complete*/, nw_error_t error) {
                if (content && self->parent.on_data_received) {
                    dispatch_data_apply(content,
                        ^bool(dispatch_data_t /*r*/, size_t /*o*/,
                              const void* buf, size_t size) {
                            if (self->parent.on_data_received)
                                self->parent.on_data_received(
                                    static_cast<const uint8_t*>(buf), size);
                            return true;
                        });
                }
                if (error) {
                    log_nw_error("[NetClient] Datagram flow error", error);
                    return;
                }
                if (self->dgram_conn)
                    self->receive_loop_datagram();
            });
    }
};

// ── NetClient method bodies ───────────────────────────────────────────────────

NetClient::NetClient()  : impl_(std::make_unique<Impl>(*this)) {}
NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const std::string& host, uint16_t port,
                        const uint8_t* ticket, size_t ticket_len)
{
    return impl_->connect(host, port, ticket, ticket_len);
}

void        NetClient::disconnect()              { impl_->disconnect(); }
std::string NetClient::get_server_fingerprint()  const { return impl_->server_fingerprint; }
bool        NetClient::is_connected()            const { return impl_->connected; }
bool        NetClient::is_connecting()           const { return impl_->connecting && !impl_->connected && !impl_->connect_failed_; }
bool        NetClient::connect_failed()          const { return impl_->connect_failed_; }

bool NetClient::send_message(protocol::ControlMessageType type,
                              const uint8_t* payload, size_t payload_len)
{
    return impl_->send_message(type, payload, payload_len);
}

bool NetClient::send_data(const uint8_t* data, size_t len, bool reliable)
{
    return impl_->send_data(data, len, reliable);
}

bool NetClient::send_video(const uint8_t* data, size_t len, bool reliable)
{
    return impl_->send_video(data, len, reliable);
}

void NetClient::open_av_streams()
{
    impl_->open_av_streams();
}

} // namespace parties::client
