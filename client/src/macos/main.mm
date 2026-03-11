#import <AppKit/AppKit.h>
#import "PartiesAppDelegate.h"

int main(int argc, const char* argv[])
{
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        PartiesAppDelegate* delegate = [[PartiesAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
