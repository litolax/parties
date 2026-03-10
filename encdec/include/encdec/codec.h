#pragma once

#include <parties/video_common.h>

#include <cstdint>

namespace parties::encdec {

// Re-export codec ID from common
using parties::VideoCodecId;

// Hardware accelerator backend
enum class Backend : uint8_t {
    NVENC,     // NVIDIA Video Codec SDK (encode)
    NVDEC,     // NVIDIA Video Codec SDK (decode)
    AMF,       // AMD Advanced Media Framework
    MFT,       // Windows Media Foundation Transform
    OpenH264,  // Cisco OpenH264 (software H.264)
    Libhevc,   // Ittiam libhevc (software H.265)
    Software,  // Software (dav1d, etc.)
};

inline const char* backend_name(Backend b) {
    switch (b) {
    case Backend::NVENC:    return "NVENC";
    case Backend::NVDEC:    return "NVDEC";
    case Backend::AMF:      return "AMF";
    case Backend::MFT:      return "MFT";
    case Backend::OpenH264: return "OpenH264";
    case Backend::Libhevc:  return "Libhevc";
    case Backend::Software: return "Software";
    }
    return "Unknown";
}

inline const char* codec_name(VideoCodecId c) {
    switch (c) {
    case VideoCodecId::AV1:  return "AV1";
    case VideoCodecId::H265: return "H.265";
    case VideoCodecId::H264: return "H.264";
    }
    return "Unknown";
}

} // namespace parties::encdec
