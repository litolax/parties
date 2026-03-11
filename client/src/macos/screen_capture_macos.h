#pragma once

// macOS screen capture using ScreenCaptureKit (macOS 12.3+).
// Delivers frames as CVPixelBufferRef (BGRA, Metal-compatible) on a
// background queue.  Interface mirrors the Windows ScreenCapture class
// closely enough that app_macos.mm can treat them the same way.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef __OBJC__
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#else
#include <CoreVideo/CVPixelBuffer.h>
#endif

namespace parties::client {

struct CaptureTargetMac {
    enum class Type { Window, Display };
    Type        type;
    std::string name;
    uint32_t    id = 0;   // SCWindow.windowID or SCDisplay.displayID
};

class ScreenCaptureMac {
public:
    ScreenCaptureMac();
    ~ScreenCaptureMac();

    // Asynchronously enumerates shareable content and calls back on the main
    // queue.  Must be called before start().
    void enumerate(std::function<void(std::vector<CaptureTargetMac>)> callback);

    // Start capturing `target` at up to `target_fps` frames/second.
    bool start(const CaptureTargetMac& target, uint32_t target_fps = 60);
    void stop();

    bool     is_capturing() const { return capturing_; }
    uint32_t width()        const { return width_;  }
    uint32_t height()       const { return height_; }

    // Called on an internal queue for each new frame.
    // pixel_buffer is in kCVPixelFormatType_32BGRA, Metal-compatible.
    // Caller must CFRetain/CFRelease if it needs to hold the buffer.
    std::function<void(CVPixelBufferRef, uint32_t width, uint32_t height)> on_frame;

    // Called when the captured window/display is removed.
    std::function<void()> on_closed;

private:
    struct Impl;
    Impl* impl_ = nullptr;

    bool     capturing_ = false;
    uint32_t width_     = 0;
    uint32_t height_    = 0;
};

} // namespace parties::client
