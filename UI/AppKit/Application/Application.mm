/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibWebView/EventLoop/EventLoopImplementationMacOS.h>
#include <Utilities/Conversions.h>

#import <Application/Application.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

namespace Ladybird {

Application::Application() = default;

Optional<ByteString> Application::ask_user_for_download_folder() const
{
    auto* panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:NO];
    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];
    [panel setMessage:@"Select download directory"];

    if ([panel runModal] != NSModalResponseOK)
        return {};

    return Ladybird::ns_string_to_byte_string([[panel URL] path]);
}

NonnullOwnPtr<Core::EventLoop> Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        Core::EventLoopManager::install(*new WebView::EventLoopManagerMacOS);
        [::Application sharedApplication];
    }

    return WebView::Application::create_platform_event_loop();
}

}

@interface Application ()
@end

@implementation Application

#pragma mark - NSApplication

- (void)terminate:(id)sender
{
    Core::EventLoop::current().quit(0);
}

- (void)sendEvent:(NSEvent*)event
{
    if ([event type] == NSEventTypeApplicationDefined) {
        Core::ThreadEventQueue::current().process();
    } else {
        [super sendEvent:event];
    }
}

@end
