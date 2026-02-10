#include <parties/codec.h>
#include <parties/audio_common.h>

#include <opus/opus.h>

namespace parties {

OpusCodec::OpusCodec() = default;

OpusCodec::~OpusCodec() {
    if (encoder_) opus_encoder_destroy(encoder_);
    if (decoder_) opus_decoder_destroy(decoder_);
}

bool OpusCodec::init_encoder(int sample_rate, int channels, int bitrate) {
    int err;
    encoder_ = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) return false;
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    return true;
}

bool OpusCodec::init_decoder(int sample_rate, int channels) {
    int err;
    decoder_ = opus_decoder_create(sample_rate, channels, &err);
    return err == OPUS_OK;
}

int OpusCodec::encode(const float* pcm_in, int frame_size,
                      uint8_t* opus_out, int max_opus_bytes) {
    if (!encoder_) return -1;
    return opus_encode_float(encoder_, pcm_in, frame_size, opus_out, max_opus_bytes);
}

int OpusCodec::decode(const uint8_t* opus_in, int opus_len,
                      float* pcm_out, int max_frame_size) {
    if (!decoder_) return -1;
    return opus_decode_float(decoder_, opus_in, opus_len, pcm_out, max_frame_size, 0);
}

void OpusCodec::set_bitrate(int bps) {
    if (encoder_) opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bps));
}

} // namespace parties
