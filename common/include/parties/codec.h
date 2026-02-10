#pragma once

#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

namespace parties {

class OpusCodec {
public:
    OpusCodec();
    ~OpusCodec();

    bool init_encoder(int sample_rate, int channels, int bitrate);
    bool init_decoder(int sample_rate, int channels);

    // Encode PCM -> Opus. Returns bytes written, or negative on error.
    int encode(const float* pcm_in, int frame_size,
               uint8_t* opus_out, int max_opus_bytes);

    // Decode Opus -> PCM. Pass nullptr for PLC. Returns samples decoded.
    int decode(const uint8_t* opus_in, int opus_len,
               float* pcm_out, int max_frame_size);

    void set_bitrate(int bps);

private:
    OpusEncoder* encoder_ = nullptr;
    OpusDecoder* decoder_ = nullptr;
};

} // namespace parties
