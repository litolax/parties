#pragma once

#include <parties/codec.h>
#include <parties/audio_common.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace parties::client {

// Captures system audio via WASAPI Application Loopback and encodes to Opus stereo.
// Uses ActivateAudioInterfaceAsync for process-specific capture (Win10 2004+).
class StreamAudioCapture {
public:
    StreamAudioCapture();
    ~StreamAudioCapture();

    // target_pid: >0 = capture only this process tree (window share)
    //             0 = capture all system audio except our own process (monitor share)
    bool init(uint32_t target_pid = 0);
    void shutdown();

    bool start();
    void stop();

    // Called from capture thread when an encoded Opus frame is ready
    std::function<void(const uint8_t* opus_data, size_t opus_len)> on_encoded_frame;

private:
    void capture_thread_func();

    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;  // stereo
    static constexpr int kFrameSize = 960;  // 20ms at 48kHz

    struct WasapiState;
    WasapiState* wasapi_ = nullptr;

    std::thread capture_thread_;
    std::atomic<bool> running_{false};

    OpusCodec encoder_;
    bool encoder_initialized_ = false;

    // Capture accumulation buffer (stereo interleaved)
    std::vector<float> capture_buf_;
    size_t capture_pos_ = 0;

    uint8_t opus_buf_[audio::MAX_OPUS_PACKET];
};

} // namespace parties::client
