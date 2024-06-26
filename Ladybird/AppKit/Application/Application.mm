/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
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

- (instancetype)init
{
    if (self = [super init]) {
        m_application_bridge = make<Ladybird::ApplicationBridge>();
    }

    return self;
}

#pragma mark - Public methods

- (ErrorOr<void>)launchRequestServer:(Vector<ByteString> const&)certificates
{
    return m_application_bridge->launch_request_server(certificates);
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
