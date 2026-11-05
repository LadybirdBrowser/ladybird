/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibWebView/QuarantineMacOS.h>

#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>

namespace WebView {

// The system records a quarantine event for the file, from which it tells the user that the file came from the web.
bool mark_as_web_download(ByteString const& path)
{
    @autoreleasepool {
        auto* url = [NSURL fileURLWithFileSystemRepresentation:path.characters() isDirectory:NO relativeToURL:nil];
        auto* properties = @ {
            (__bridge NSString*)kLSQuarantineAgentNameKey : @"Ladybird",
            (__bridge NSString*)kLSQuarantineTypeKey : (__bridge NSString*)kLSQuarantineTypeWebDownload,
        };

        NSError* error = nil;
        if ([url setResourceValue:properties forKey:NSURLQuarantinePropertiesKey error:&error])
            return true;

        dbgln("Unable to mark {} as a web download: {}", path, error.localizedDescription.UTF8String);
        return false;
    }
}

}
