find_package(dav1d CONFIG REQUIRED)
find_package(libhevc CONFIG REQUIRED)
find_path(OPENH264_INCLUDE_DIRS "wels/codec_api.h")
find_library(OPENH264_LIBRARY openh264)

add_library(parties_encdec STATIC
    src/factory.cpp
    src/nvidia/nvidia_loader.cpp
    src/nvidia/nvenc_encoder.cpp
    src/nvidia/nvdec_decoder.cpp
    src/amd/amf_loader.cpp
    src/amd/amf_encoder.cpp
    src/amd/amf_decoder.cpp
    src/mft/mft_encoder.cpp
    src/mft/mft_decoder.cpp
    src/dav1d/dav1d_decoder.cpp
    src/openh264/openh264_encoder.cpp
    src/openh264/openh264_decoder.cpp
    src/libhevc/libhevc_decoder.cpp
)

target_include_directories(parties_encdec
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${OPENH264_INCLUDE_DIRS}
)

target_link_libraries(parties_encdec
    PUBLIC
        parties_common
    PRIVATE
        dav1d::dav1d
        libhevc::decoder
        ${OPENH264_LIBRARY}
        d3d11 mfplat mfuuid mf strmiids
)
