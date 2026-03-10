// AMF hardware encoder wrapper using D3D11 device directly.
// Falls back gracefully — caller should try MFT if init() returns false.
#pragma once

#include <AMF/core/Factory.h>
#include <AMF/core/Context.h>
#include <AMF/components/Component.h>
#include <parties/video_common.h>

#include <cstdint>
#include <functional>
#include <d3d11.h>
#include <wrl/client.h>

namespace parties::client::amd {

class AmfEncoder {
public:
    AmfEncoder();
    ~AmfEncoder();

    // Initialize AMF encoder with a D3D11 device. Returns false if AMF unavailable.
    bool init(ID3D11Device* device, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate,
              parties::VideoCodecId preferred_codec);
    void shutdown();

    // Encode a BGRA texture (wraps as AMF surface).
    bool encode_frame(ID3D11Texture2D* bgra_texture, int64_t timestamp_100ns);

    // Zero-copy: register caller-owned textures, then encode directly from them.
    static constexpr int MAX_EXTERNAL_INPUTS = 4;
    int register_input(ID3D11Texture2D* texture);
    void unregister_inputs();
    bool encode_registered(int slot, int64_t timestamp_100ns);

    void force_keyframe();
    void set_bitrate(uint32_t bitrate);

    parties::VideoCodecId codec() const { return codec_; }

    // Callback with encoded bitstream
    std::function<void(const uint8_t* data, size_t len, bool keyframe)> on_encoded;

private:
    bool try_codec(const wchar_t* component_id, parties::VideoCodecId id);
    bool do_encode(ID3D11Texture2D* texture, int64_t timestamp_100ns);

    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* encoder_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;

    // External registered inputs (zero-copy path)
    ID3D11Texture2D* external_inputs_[MAX_EXTERNAL_INPUTS]{};
    int num_external_inputs_ = 0;

    parties::VideoCodecId codec_ = parties::VideoCodecId::H264;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    bool initialized_ = false;
    bool force_keyframe_ = false;
};

} // namespace parties::client::amd
