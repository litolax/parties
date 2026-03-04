vcpkg_from_github(
    OUT_SOURCE_PATH QUIC_SOURCE_PATH
    REPO microsoft/msquic
    REF "v${VERSION}"
    SHA512 a16ec3de6a0a68256a4688586d6205ef9ddc4ea22a3ea2b208d4b953faf4369a0dd573b9e27bb933344f37dbfb421fa6f819a70e87a7d02a0b4971adde60dfdb
    HEAD_REF main
)

# XDP headers are needed for the Windows build even if XDP isn't used at runtime
vcpkg_from_github(
    OUT_SOURCE_PATH XDP_WINDOWS
    REPO microsoft/xdp-for-windows
    REF v1.1.3
    SHA512 8bf38182bf3c2da490e6e4df9420bacc3839e19d7cea6ca1c1420d1fd349e87a1f80992b52524eaab70a84ff1ac4e1681974211871117847fba92334350dcf13
    HEAD_REF main
)
if(NOT EXISTS "${QUIC_SOURCE_PATH}/submodules/xdp-for-windows/published/external")
    file(REMOVE_RECURSE "${QUIC_SOURCE_PATH}/submodules/xdp-for-windows")
    file(COPY "${XDP_WINDOWS}/published/external" DESTINATION "${QUIC_SOURCE_PATH}/submodules/xdp-for-windows/published")
endif()

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" STATIC_CRT)

# MsQuic unconditionally appends /GL (LTCG) to release flags in its CMakeLists.txt.
# lld-link (clang-cl) cannot consume LTCG bitcode objects. Strip /GL and /LTCG.
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /GL /Zi")]]
    [[set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Zi")]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /GL /Zi")]]
    [[set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG /IGNORE:4075]]
    [[${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /IGNORE:4075]]
)
vcpkg_replace_string("${QUIC_SOURCE_PATH}/CMakeLists.txt"
    [[${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /IGNORE:4075]]
    [[${CMAKE_EXE_LINKER_FLAGS_RELEASE} /IGNORE:4075]]
)

vcpkg_cmake_configure(
    SOURCE_PATH "${QUIC_SOURCE_PATH}"
    OPTIONS
        -DQUIC_TLS_LIB=schannel
        -DQUIC_BUILD_SHARED=OFF
        -DQUIC_SOURCE_LINK=OFF
        -DQUIC_BUILD_PERF=OFF
        -DQUIC_BUILD_TEST=OFF
        -DQUIC_BUILD_TOOLS=OFF
        -DQUIC_ENABLE_LOGGING=OFF
        "-DQUIC_STATIC_LINK_CRT=${STATIC_CRT}"
        "-DQUIC_STATIC_LINK_PARTIAL_CRT=${STATIC_CRT}"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# MsQuic's static build doesn't generate proper CMake export targets.
# Replace the broken auto-generated config with our custom one.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/msquic"
)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/msquic-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/msquic")

vcpkg_install_copyright(FILE_LIST "${QUIC_SOURCE_PATH}/LICENSE")
