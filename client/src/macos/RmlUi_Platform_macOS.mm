#import "RmlUi_Platform_macOS.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <RmlUi/Core/Log.h>

SystemInterface_macOS::SystemInterface_macOS() = default;
SystemInterface_macOS::~SystemInterface_macOS() = default;

double SystemInterface_macOS::GetElapsedTime()
{
    return CACurrentMediaTime();
}

bool SystemInterface_macOS::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    const char* prefix = "";
    switch (type) {
        case Rml::Log::LT_ERROR:   prefix = "[RmlUi][ERROR] "; break;
        case Rml::Log::LT_WARNING: prefix = "[RmlUi][WARN]  "; break;
        case Rml::Log::LT_INFO:    prefix = "[RmlUi][INFO]  "; break;
        case Rml::Log::LT_DEBUG:   prefix = "[RmlUi][DEBUG] "; break;
        default: break;
    }
    NSLog(@"%s%s", prefix, message.c_str());
    return true;
}

void SystemInterface_macOS::SetClipboardText(const Rml::String& text)
{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:text.c_str()]
          forType:NSPasteboardTypeString];
}

void SystemInterface_macOS::GetClipboardText(Rml::String& text)
{
    NSString* str = [[NSPasteboard generalPasteboard]
                         stringForType:NSPasteboardTypeString];
    text = str ? str.UTF8String : "";
}
