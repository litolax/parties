#pragma once

#include <encdec/decoder.h>
#include "nvidia_loader.h"

#include <cstdint>
#include <vector>

namespace parties::encdec::nvidia {

class NvdecDecoder final : public Decoder {
public:
    NvdecDecoder();
    ~NvdecDecoder() override;

    bool init(VideoCodecId codec, uint32_t width, uint32_t height);

    bool decode(const uint8_t* data, size_t len, int64_t timestamp) override;
    void flush() override;
    bool context_lost() const override { return context_lost_; }
    DecoderInfo info() const override;

private:
    static int CUDAAPI handle_sequence(void* user, CUVIDEOFORMAT* fmt);
    static int CUDAAPI handle_decode(void* user, CUVIDPICPARAMS* pic);
    static int CUDAAPI handle_display(void* user, CUVIDPARSERDISPINFO* info);

    int on_sequence(CUVIDEOFORMAT* fmt);
    int on_decode(CUVIDPICPARAMS* pic);
    int on_display(CUVIDPARSERDISPINFO* info);

    CudaApi cuda_{};
    CuvidApi cuvid_{};

    CUcontext cu_ctx_ = nullptr;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;

    VideoCodecId codec_ = VideoCodecId::AV1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bit_depth_ = 8;
    uint32_t num_decode_surfaces_ = 0;

    uint8_t* pinned_nv12_ = nullptr;
    size_t pinned_nv12_size_ = 0;

    bool initialized_ = false;
    bool context_lost_ = false;
};

} // namespace parties::encdec::nvidia
