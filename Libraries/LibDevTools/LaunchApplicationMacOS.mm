/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if !defined(AK_OS_MACOS)
static_assert(false, "This file must only be used for macOS");
#endif

#include <LibDevTools/LaunchApplicationMacOS.h>

#import <AppKit/AppKit.h>

namespace DevTools {

ErrorOr<pid_t> launch_macos_application(StringView bundle_path, Vector<ByteString> const& arguments)
{
    @autoreleasepool {
        auto* path = [[NSString alloc] initWithBytes:bundle_path.characters_without_null_termination()
                                              length:bundle_path.length()
                                            encoding:NSUTF8StringEncoding];
        if (path == nil)
            return Error::from_string_literal("Application path is not valid UTF-8");

        auto* application_arguments = [NSMutableArray arrayWithCapacity:arguments.size()];
        for (auto const& argument : arguments) {
            auto* value = [NSString stringWithUTF8String:argument.characters()];
            if (value == nil)
                return Error::from_string_literal("Application argument is not valid UTF-8");
            [application_arguments addObject:value];
        }

        auto* configuration = [NSWorkspaceOpenConfiguration configuration];
        configuration.createsNewApplicationInstance = YES;
        configuration.arguments = application_arguments;

        __block pid_t launched_pid = -1;
        auto* semaphore = dispatch_semaphore_create(0);

        [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:path isDirectory:YES]
                                            configuration:configuration
                                        completionHandler:^(NSRunningApplication* application, NSError* error) {
                                            if (error == nil && application != nil)
                                                launched_pid = application.processIdentifier;
                                            dispatch_semaphore_signal(semaphore);
                                        }];

        // The completion handler arrives on a queue of its own — so waiting here is safe. LaunchServices answers
        // promptly; the deadline is only so that a launch which never comes back can't wedge the caller for good.
        auto deadline = dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(semaphore, deadline) != 0)
            return Error::from_string_literal("Timed out waiting for the application to launch");

        if (launched_pid <= 0)
            return Error::from_string_literal("Unable to launch the application");

        return launched_pid;
    }
}

}
