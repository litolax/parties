vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ittiam-systems/libhevc
    REF v${VERSION}
    SHA512 22e34f5d39eb2ca1ee0bed698bdfb3e552bf307a21e264f4be2701d9ec2be7e4fff08029ad74be00b84b59818850b4123ca7f52e5afbdecc898784075dc9be04
    HEAD_REF main
)

# Replace upstream CMakeLists.txt with our vcpkg-friendly version
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(MAKE_DIRECTORY "${SOURCE_PATH}/cmake")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/libhevc-config.cmake.in" DESTINATION "${SOURCE_PATH}/cmake")

# Win32 threading implementation (upstream only supports pthreads)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/ithread_win32.c" DESTINATION "${SOURCE_PATH}/common")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME libhevc
    CONFIG_PATH lib/cmake/libhevc
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
