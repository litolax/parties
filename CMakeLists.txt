cmake_minimum_required(VERSION 3.20)
project(miniaudio-rnnoise C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- RNNoise (from vcpkg overlay port) ---
find_package(rnnoise CONFIG REQUIRED)

# --- Main executable ---
add_executable(${PROJECT_NAME} src/main.c)
target_link_libraries(${PROJECT_NAME} PRIVATE rnnoise::rnnoise)
