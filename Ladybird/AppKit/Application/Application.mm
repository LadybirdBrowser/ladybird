/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Application/ApplicationBridge.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibWebView/WebContentClient.h>

#import <Application/Application.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

@interface Application ()
{
    OwnPtr<Ladybird::ApplicationBridge> m_application_bridge;
}

@end

@implementation Application

#pragma mark - Public methods

- (void)setupWebViewApplication:(Main::Arguments&)arguments
                  newTabPageURL:(URL::URL)new_tab_page_url
{
    m_application_bridge = Ladybird::ApplicationBridge::create(arguments, move(new_tab_page_url));
}

- (ErrorOr<void>)launchRequestServer
{
    return m_application_bridge->launch_request_server();
}

- (ErrorOr<void>)launchImageDecoder
{
    return m_application_bridge->launch_image_decoder();
}

- (ErrorOr<NonnullRefPtr<WebView::WebContentClient>>)launchWebContent:(Ladybird::WebViewBridge&)web_view_bridge
{
    return m_application_bridge->launch_web_content(web_view_bridge);
}

- (ErrorOr<IPC::File>)launchWebWorker
{
    return m_application_bridge->launch_web_worker();
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
