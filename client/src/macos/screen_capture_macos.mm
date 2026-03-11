// macOS screen capture — ScreenCaptureKit backend.
//
// ScreenCaptureKit requires macOS 12.3+.
// Privacy: the app must have the Screen Recording entitlement (or prompt the
// user at runtime via SCShareableContent which triggers the permission dialog
// automatically).

#include "screen_capture_macos.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>

// ── SCStreamOutput delegate ───────────────────────────────────────────────────

@interface PartiesCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) std::function<void(CVPixelBufferRef, uint32_t, uint32_t)> onFrame;
@property (nonatomic, assign) std::function<void()> onClosed;
@end

@implementation PartiesCaptureOutput

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen) return;

    CVPixelBufferRef pixel_buf =
        CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixel_buf) return;

    uint32_t w = (uint32_t)CVPixelBufferGetWidth(pixel_buf);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(pixel_buf);

    if (self.onFrame) {
        CFRetain(pixel_buf);
        self.onFrame(pixel_buf, w, h);
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    if (error)
        NSLog(@"[ScreenCaptureMac] Stream stopped: %@", error.localizedDescription);
    if (self.onClosed)
        self.onClosed();
}

@end

// ── Impl ─────────────────────────────────────────────────────────────────────

namespace parties::client {

struct ScreenCaptureMac::Impl {
    SCStream*             stream  = nullptr;
    PartiesCaptureOutput* output  = nullptr;
    dispatch_queue_t      queue   = nullptr;
};

ScreenCaptureMac::ScreenCaptureMac()
    : impl_(new Impl())
{
    impl_->queue = dispatch_queue_create("com.parties.screencapture",
                                         DISPATCH_QUEUE_SERIAL);
    impl_->output = [[PartiesCaptureOutput alloc] init];
}

ScreenCaptureMac::~ScreenCaptureMac()
{
    stop();
    if (impl_->queue) {
        dispatch_release(impl_->queue);
        impl_->queue = nullptr;
    }
    delete impl_;
}

// ── enumerate ─────────────────────────────────────────────────────────────────

void ScreenCaptureMac::enumerate(
    std::function<void(std::vector<CaptureTargetMac>)> callback)
{
    [SCShareableContent
        getShareableContentWithCompletionHandler:
        ^(SCShareableContent* content, NSError* error) {
            if (error) {
                NSLog(@"[ScreenCaptureMac] getShareableContent error: %@",
                      error.localizedDescription);
                callback({});
                return;
            }

            std::vector<CaptureTargetMac> targets;

            // Displays first
            for (SCDisplay* d in content.displays) {
                CaptureTargetMac t;
                t.type = CaptureTargetMac::Type::Display;
                t.name = [NSString stringWithFormat:@"Display %u",
                          (unsigned)d.displayID].UTF8String;
                t.id   = d.displayID;
                targets.push_back(std::move(t));
            }

            // Visible, on-screen windows
            for (SCWindow* w in content.windows) {
                if (!w.onScreen || w.frame.size.width < 100) continue;
                CaptureTargetMac t;
                t.type = CaptureTargetMac::Type::Window;
                t.name = w.title ? w.title.UTF8String
                                 : (w.owningApplication.applicationName
                                        ? w.owningApplication.applicationName.UTF8String
                                        : "Untitled");
                t.id   = w.windowID;
                targets.push_back(std::move(t));
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                callback(std::move(targets));
            });
        }];
}

// ── start ─────────────────────────────────────────────────────────────────────

bool ScreenCaptureMac::start(const CaptureTargetMac& target, uint32_t target_fps)
{
    if (capturing_) stop();

    // Wire callbacks into the delegate.
    // Plain pointer capture — C++ lambdas do not support __block.
    ScreenCaptureMac* cap = this;
    impl_->output.onFrame = [cap](CVPixelBufferRef buf, uint32_t w, uint32_t h) {
        cap->width_  = w;
        cap->height_ = h;
        if (cap->on_frame) cap->on_frame(buf, w, h);
        CFRelease(buf);
    };
    impl_->output.onClosed = [cap]() {
        cap->capturing_ = false;
        if (cap->on_closed) cap->on_closed();
    };

    // We need SCShareableContent to build the filter.
    // Capture synchronously via a semaphore so start() has a clear success/fail return.
    __block bool success = false;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    uint32_t target_id = target.id;
    bool     is_display = (target.type == CaptureTargetMac::Type::Display);

    [SCShareableContent
        getShareableContentWithCompletionHandler:
        ^(SCShareableContent* content, NSError* error) {
            if (error) {
                NSLog(@"[ScreenCaptureMac] getShareableContent: %@",
                      error.localizedDescription);
                dispatch_semaphore_signal(sem);
                return;
            }

            SCContentFilter* filter = nil;

            if (is_display) {
                for (SCDisplay* d in content.displays) {
                    if (d.displayID == target_id) {
                        filter = [[SCContentFilter alloc]
                                    initWithDisplay:d
                                  excludingWindows:@[]];
                        break;
                    }
                }
            } else {
                for (SCWindow* w in content.windows) {
                    if (w.windowID == target_id) {
                        filter = [[SCContentFilter alloc]
                                    initWithDesktopIndependentWindow:w];
                        break;
                    }
                }
            }

            if (!filter) {
                NSLog(@"[ScreenCaptureMac] Target %u not found in shareable content",
                      target_id);
                dispatch_semaphore_signal(sem);
                return;
            }

            // Stream configuration
            SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
            cfg.minimumFrameInterval =
                CMTimeMake(1, (int32_t)(target_fps > 0 ? target_fps : 60));
            cfg.pixelFormat          = kCVPixelFormatType_32BGRA;
            cfg.colorSpaceName       = kCGColorSpaceSRGB;
            cfg.showsCursor          = NO;

            // Start with the display/window's natural resolution.
            // The encoder will decide the actual encode resolution.
            if (is_display) {
                for (SCDisplay* d in content.displays) {
                    if (d.displayID == target_id) {
                        cfg.width  = (size_t)d.width;
                        cfg.height = (size_t)d.height;
                        break;
                    }
                }
            }

            SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                                  configuration:cfg
                                                       delegate:cap->impl_->output];

            NSError* addErr = nil;
            [stream addStreamOutput:cap->impl_->output
                               type:SCStreamOutputTypeScreen
                 sampleHandlerQueue:cap->impl_->queue
                              error:&addErr];
            if (addErr) {
                NSLog(@"[ScreenCaptureMac] addStreamOutput: %@",
                      addErr.localizedDescription);
                dispatch_semaphore_signal(sem);
                return;
            }

            [stream startCaptureWithCompletionHandler:^(NSError* startErr) {
                if (startErr) {
                    NSLog(@"[ScreenCaptureMac] startCapture: %@",
                          startErr.localizedDescription);
                } else {
                    cap->impl_->stream = stream;
                    cap->capturing_    = true;
                    success             = true;
                }
                dispatch_semaphore_signal(sem);
            }];
        }];

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return success;
}

// ── stop ──────────────────────────────────────────────────────────────────────

void ScreenCaptureMac::stop()
{
    if (!impl_->stream) return;

    SCStream* stream = impl_->stream;
    impl_->stream    = nullptr;
    capturing_       = false;

    [stream stopCaptureWithCompletionHandler:^(NSError* err) {
        if (err)
            NSLog(@"[ScreenCaptureMac] stopCapture: %@", err.localizedDescription);
    }];
}

} // namespace parties::client
