#include <client/video_decoder.h>
#include <encdec/factory.h>

#include <cstdio>
#include <parties/profiler.h>

namespace parties::client {

VideoDecoder::~VideoDecoder() { shutdown(); }

bool VideoDecoder::init(VideoCodecId codec, uint32_t width, uint32_t height) {
	ZoneScopedN("VideoDecoder::init");
    shutdown();
    codec_ = codec;
    width_ = width;
    height_ = height;

    if (!hardware_disabled_) {
        // Full chain: NVDEC → AMF → dav1d/MFT
        decoder_ = encdec::create_decoder(codec, width, height);
    } else {
        // Software only (after GPU context loss)
        decoder_ = encdec::create_software_decoder(codec, width, height);
    }

    if (!decoder_) return false;
    initialized_ = true;
    return true;
}

void VideoDecoder::shutdown() {
    decoder_.reset();
    initialized_ = false;
}

bool VideoDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
	ZoneScopedN("VideoDecoder::decode");
    if (!initialized_) return false;
    decoder_->on_decoded = on_decoded;
    return decoder_->decode(data, len, timestamp);
}

void VideoDecoder::flush() {
	ZoneScopedN("VideoDecoder::flush");
    if (!initialized_) return;
    decoder_->on_decoded = on_decoded;
    decoder_->flush();
}

bool VideoDecoder::context_lost() const {
    if (decoder_) return decoder_->context_lost();
    return false;
}

const char* VideoDecoder::backend_name() const {
    if (decoder_) return encdec::backend_name(decoder_->info().backend);
    return "none";
}

} // namespace parties::client
