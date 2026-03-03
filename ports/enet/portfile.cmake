vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zpl-c/enet
    REF v2.6.5
    SHA512 9f84279abce488d569db90ed64bb88a8c4daf0879771c6229a37cd168320b8f7ca8160a00a5e71f573cccd74c45934a4b1eacb889836003883c8f79c433012ab
    HEAD_REF master
)

# Fix: on iOS the system SDK already provides clock_gettime (since iOS 10).
# enet.h's POSIX fallback is guarded by __MAC_OS_X_VERSION_MIN_REQUIRED < 101200,
# but __APPLE__ is also true on iOS and the declaration clashes with the SDK.
# Step 1: ensure TargetConditionals.h is included BEFORE the #ifdef _WIN32 block
#         so TARGET_OS_IPHONE is defined when the #elif __APPLE__ branch is reached.
# Step 2: add !TARGET_OS_IPHONE to the guard.
vcpkg_replace_string(
    "${SOURCE_PATH}/include/enet.h"
    "    #define internal_clock_gettime clock_gettime\n\n    #ifdef _WIN32"
    "    #define internal_clock_gettime clock_gettime\n\n    #ifdef __APPLE__\n    #include <TargetConditionals.h>\n    #endif\n\n    #ifdef _WIN32"
)
vcpkg_replace_string(
    "${SOURCE_PATH}/include/enet.h"
    "#elif __APPLE__ && __MAC_OS_X_VERSION_MIN_REQUIRED < 101200"
    "#elif __APPLE__ && !TARGET_OS_IPHONE && __MAC_OS_X_VERSION_MIN_REQUIRED < 101200"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENET_STATIC=ON
        -DENET_SHARED=OFF
        -DENET_TEST=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME enet CONFIG_PATH lib/cmake/enet)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
