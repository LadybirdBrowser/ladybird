/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Interface/LadybirdWebViewBridge.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibWebView/Application.h>
#include <Utilities/Conversions.h>

#import <Application/Application.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

namespace Ladybird {

class ApplicationBridge : public WebView::Application {
    WEB_VIEW_APPLICATION(ApplicationBridge)

private:
    virtual Optional<ByteString> ask_user_for_download_folder() const override
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
};

ApplicationBridge::ApplicationBridge(Badge<WebView::Application>, Main::Arguments&)
{
}

}

@interface Application ()
{
    OwnPtr<Ladybird::ApplicationBridge> m_application_bridge;

    RefPtr<Requests::RequestClient> m_request_server_client;
    RefPtr<ImageDecoderClient::Client> m_image_decoder_client;
}

@end

@implementation Application

#pragma mark - Public methods

- (void)setupWebViewApplication:(Main::Arguments&)arguments
{
    m_application_bridge = Ladybird::ApplicationBridge::create(arguments);
}

- (ErrorOr<void>)launchServices
{
    TRY(m_application_bridge->launch_services());
    return {};
}

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
