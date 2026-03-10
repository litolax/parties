// AMF hardware decoder for AV1/H.265/H.264.
// Falls back gracefully — caller should try software decoders if init() returns false.
#pragma once

#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/components/Component.h>
#include <client/video_decoder.h>
#include <parties/video_common.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace parties::client::amd {

class AmfDecoder {
public:
    AmfDecoder();
    ~AmfDecoder();

    bool init(parties::VideoCodecId codec, uint32_t width, uint32_t height);
    void shutdown();
    bool decode(const uint8_t* data, size_t len, int64_t timestamp);
    void flush();
    bool context_lost() const { return context_lost_; }

    std::function<void(const parties::client::DecodedFrame& frame)> on_decoded;

private:
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* decoder_ = nullptr;

    parties::VideoCodecId codec_ = parties::VideoCodecId::AV1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
    bool context_lost_ = false;

    std::vector<uint8_t> host_buffer_;
};

} // namespace parties::client::amd
