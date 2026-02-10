#pragma once

namespace parties::audio {

constexpr int SAMPLE_RATE      = 48000;
constexpr int CHANNELS         = 1;
constexpr int FRAME_SIZE       = 480;     // 10ms at 48kHz (matches RNNoise)
constexpr int OPUS_FRAME_SIZE  = 960;     // 20ms at 48kHz (2x RNNoise frames)
constexpr int OPUS_BITRATE     = 32000;   // 32 kbps default
constexpr int MAX_OPUS_PACKET  = 512;     // max bytes per opus frame

} // namespace parties::audio
