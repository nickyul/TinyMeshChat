#include "window_source_filter.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

namespace tmc {
ApplicationWindowTitles applicationWindowTitles() {
    ApplicationWindowTitles titles;
    @autoreleasepool {
        NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
        for (NSDictionary* info in windows) {
            NSString* name = info[(id)kCGWindowName];
            if (!name.length) continue;
            const auto title = QString::fromUtf8(name.UTF8String);
            NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:
                [info[(id)kCGWindowOwnerPID] intValue]];
            CGRect bounds = CGRectZero;
            NSDictionary* boundsInfo = info[(id)kCGWindowBounds];
            const bool validBounds = boundsInfo && CGRectMakeWithDictionaryRepresentation(
                (__bridge CFDictionaryRef)boundsInfo, &bounds);
            const bool allowed = app && !app.terminated && !app.hidden
                && app.activationPolicy == NSApplicationActivationPolicyRegular
                && [info[(id)kCGWindowLayer] intValue] == 0
                && [info[(id)kCGWindowAlpha] doubleValue] > 0
                && validBounds && bounds.size.width > 0 && bounds.size.height > 0;
            const auto appName = QString::fromUtf8(app.localizedName.UTF8String);
            const auto label = appName.isEmpty() || appName == title ? title : appName + " — " + title;
            rememberWindowTitle(titles, title, label, allowed);
        }
    }
    return titles;
}
} // namespace tmc
