#pragma once

#include <encdec/codec.h>

#include <cstdint>
#include <functional>

namespace parties::encdec {

struct DecodedFrame {
    const uint8_t* y_plane;
    const uint8_t* u_plane;   // I420: U plane; NV12: interleaved UV plane
    const uint8_t* v_plane;   // I420: V plane; NV12: unused (nullptr)
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t width;
    uint32_t height;
    int64_t timestamp;
    bool nv12 = false;        // true = NV12 (Y + interleaved UV), false = I420 (Y + U + V)
};

struct DecoderInfo {
    Backend backend;
    VideoCodecId codec;
};

class Decoder {
public:
    virtual ~Decoder() = default;

    // Feed encoded data. Fires on_decoded when a frame is ready.
    virtual bool decode(const uint8_t* data, size_t len, int64_t timestamp) = 0;

    // Flush any buffered frames
    virtual void flush() = 0;

    // True if the GPU context was invalidated (device reset, game launch, TDR).
    // Caller should destroy this decoder and create a new one.
    virtual bool context_lost() const { return false; }

    // Metadata about the active decoder
    virtual DecoderInfo info() const = 0;

    // Callback with decoded frame
    std::function<void(const DecodedFrame& frame)> on_decoded;

    // Convenience
    VideoCodecId codec() const { return info().codec; }
};

} // namespace parties::encdec
