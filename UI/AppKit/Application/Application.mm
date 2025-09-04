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
#import <Application/ApplicationDelegate.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Tab.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

namespace Ladybird {

Application::Application() = default;

NonnullOwnPtr<Core::EventLoop> Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        Core::EventLoopManager::install(*new WebView::EventLoopManagerMacOS);
        [::Application sharedApplication];
    }

    return WebView::Application::create_platform_event_loop();
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    if (auto* tab = [delegate activeTab])
        return [[tab web_view] view];
    return {};
}

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

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    auto message = MUST(String::formatted("{} saved to: {}", download_name, path));

    auto* dialog = [[NSAlert alloc] init];
    [dialog setMessageText:Ladybird::string_to_ns_string(message)];
    [[dialog addButtonWithTitle:@"OK"] setTag:NSModalResponseOK];
    [[dialog addButtonWithTitle:@"Open folder"] setTag:NSModalResponseContinue];

    __block auto* ns_path = Ladybird::string_to_ns_string(path.string());

    [dialog beginSheetModalForWindow:[delegate activeTab]
                   completionHandler:^(NSModalResponse response) {
                       if (response == NSModalResponseContinue) {
                           [[NSWorkspace sharedWorkspace] selectFile:ns_path inFileViewerRootedAtPath:@""];
                       }
                   }];
}

void Application::display_error_dialog(StringView error_message) const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    auto* dialog = [[NSAlert alloc] init];
    [dialog setMessageText:Ladybird::string_to_ns_string(error_message)];

    [dialog beginSheetModalForWindow:[delegate activeTab]
                   completionHandler:nil];
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
