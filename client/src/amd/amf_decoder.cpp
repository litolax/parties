#include "amf_decoder.h"
#include "amf_loader.h"

#include <AMF/components/VideoDecoderUVD.h>
#include <AMF/core/Surface.h>
#include <AMF/core/Plane.h>

#include <parties/profiler.h>
#include <cstdio>
#include <cstring>

namespace parties::client::amd {

static const wchar_t* codec_component_id(parties::VideoCodecId codec) {
    switch (codec) {
    case parties::VideoCodecId::AV1:  return AMFVideoDecoderHW_AV1;
    case parties::VideoCodecId::H265: return AMFVideoDecoderHW_H265_HEVC;
    case parties::VideoCodecId::H264: return AMFVideoDecoderUVD_H264_AVC;
    default:                          return AMFVideoDecoderHW_AV1;
    }
}

static const char* codec_name(parties::VideoCodecId codec) {
    switch (codec) {
    case parties::VideoCodecId::AV1:  return "AV1";
    case parties::VideoCodecId::H265: return "H.265";
    case parties::VideoCodecId::H264: return "H.264";
    default:                          return "unknown";
    }
}

AmfDecoder::AmfDecoder() = default;

AmfDecoder::~AmfDecoder() {
    shutdown();
}

bool AmfDecoder::init(parties::VideoCodecId codec, uint32_t width, uint32_t height) {
    ZoneScopedN("AmfDecoder::init");
    if (initialized_) return false;

    if (!load_amf(factory_)) return false;

    AMF_RESULT res = factory_->CreateContext(&context_);
    if (res != AMF_OK || !context_) {
        std::fprintf(stderr, "[AMF] Decoder CreateContext failed: %d\n", res);
        return false;
    }

    // Init D3D11 with default AMD GPU (NULL = auto-select)
    res = context_->InitDX11(nullptr);
    if (res != AMF_OK) {
        std::fprintf(stderr, "[AMF] Decoder InitDX11 failed: %d\n", res);
        context_->Release();
        context_ = nullptr;
        return false;
    }

    const wchar_t* comp_id = codec_component_id(codec);
    res = factory_->CreateComponent(context_, comp_id, &decoder_);
    if (res != AMF_OK || !decoder_) {
        std::fprintf(stderr, "[AMF] Decoder CreateComponent(%s) failed: %d\n",
                     codec_name(codec), res);
        context_->Release();
        context_ = nullptr;
        return false;
    }

    // Set low-latency mode
    decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE,
        static_cast<amf_int64>(AMF_VIDEO_DECODER_MODE_LOW_LATENCY));

    // Use actual dimensions if provided, or small default
    uint32_t w = width > 0 ? width : 1920;
    uint32_t h = height > 0 ? height : 1080;

    res = decoder_->Init(amf::AMF_SURFACE_NV12, w, h);
    if (res != AMF_OK) {
        std::fprintf(stderr, "[AMF] Decoder Init(%s, %ux%u) failed: %d\n",
                     codec_name(codec), w, h, res);
        decoder_->Release();
        decoder_ = nullptr;
        context_->Release();
        context_ = nullptr;
        return false;
    }

    codec_ = codec;
    width_ = w;
    height_ = h;

    std::fprintf(stderr, "[AMF] Initialized %s decoder (%ux%u)\n",
                 codec_name(codec), w, h);
    initialized_ = true;
    return true;
}

void AmfDecoder::shutdown() {
    if (!initialized_ && !context_lost_) return;

    if (decoder_) {
        if (!context_lost_) {
            decoder_->Drain();
            decoder_->Terminate();
        }
        decoder_->Release();
        decoder_ = nullptr;
    }

    if (context_) {
        if (!context_lost_)
            context_->Terminate();
        context_->Release();
        context_ = nullptr;
    }

    initialized_ = false;
    context_lost_ = false;
}

bool AmfDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("AmfDecoder::decode");
    if (!initialized_ || context_lost_) return false;

    // Wrap compressed data in AMF buffer
    amf::AMFBuffer* buffer = nullptr;
    AMF_RESULT res = context_->AllocBuffer(amf::AMF_MEMORY_HOST, len, &buffer);
    if (res != AMF_OK || !buffer) {
        std::fprintf(stderr, "[AMF] AllocBuffer failed: %d\n", res);
        return false;
    }

    std::memcpy(buffer->GetNative(), data, len);
    buffer->SetPts(timestamp);

    res = decoder_->SubmitInput(buffer);
    buffer->Release();

    if (res != AMF_OK && res != AMF_DECODER_NO_FREE_SURFACES) {
        std::fprintf(stderr, "[AMF] Decoder SubmitInput failed: %d\n", res);
        if (res == AMF_FAIL || res == AMF_NOT_INITIALIZED) {
            context_lost_ = true;
        }
        return false;
    }

    // Poll for decoded output
    while (true) {
        amf::AMFData* out_data = nullptr;
        res = decoder_->QueryOutput(&out_data);
        if (res == AMF_REPEAT || !out_data) break;
        if (res != AMF_OK) break;

        amf::AMFSurface* surface = nullptr;
        out_data->QueryInterface(amf::AMFSurface::IID(), reinterpret_cast<void**>(&surface));
        if (!surface) {
            out_data->Release();
            continue;
        }

        // Copy to host memory for the decoded frame callback
        res = surface->Convert(amf::AMF_MEMORY_HOST);
        if (res != AMF_OK) {
            surface->Release();
            out_data->Release();
            continue;
        }

        amf::AMFPlane* y_plane = surface->GetPlane(amf::AMF_PLANE_Y);
        amf::AMFPlane* uv_plane = surface->GetPlane(amf::AMF_PLANE_UV);

        if (y_plane && uv_plane && on_decoded) {
            DecodedFrame frame{};
            frame.y_plane = static_cast<const uint8_t*>(y_plane->GetNative());
            frame.u_plane = static_cast<const uint8_t*>(uv_plane->GetNative());
            frame.v_plane = nullptr;
            frame.y_stride = static_cast<uint32_t>(y_plane->GetHPitch());
            frame.uv_stride = static_cast<uint32_t>(uv_plane->GetHPitch());
            frame.width = static_cast<uint32_t>(y_plane->GetWidth());
            frame.height = static_cast<uint32_t>(y_plane->GetVPitch());
            frame.timestamp = out_data->GetPts();
            frame.nv12 = true;

            on_decoded(frame);
        }

        surface->Release();
        out_data->Release();
    }

    return true;
}

void AmfDecoder::flush() {
    if (!initialized_ || !decoder_ || context_lost_) return;
    decoder_->Drain();
    // Consume remaining output
    while (true) {
        amf::AMFData* data = nullptr;
        AMF_RESULT res = decoder_->QueryOutput(&data);
        if (res != AMF_OK || !data) break;
        data->Release();
    }
    decoder_->ReInit(width_, height_);
}

} // namespace parties::client::amd
