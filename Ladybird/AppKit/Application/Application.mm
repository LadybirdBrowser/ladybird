/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebContentClient.h>
#include <UI/LadybirdWebViewBridge.h>
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
                  newTabPageURL:(URL::URL)new_tab_page_url
{
    m_application_bridge = Ladybird::ApplicationBridge::create(arguments, move(new_tab_page_url));
}

- (ErrorOr<void>)launchRequestServer
{
    auto request_server_paths = TRY(get_paths_for_helper_process("RequestServer"sv));
    m_request_server_client = TRY(launch_request_server_process(request_server_paths, s_ladybird_resource_root));

    return {};
}

static ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_new_image_decoder()
{
    auto image_decoder_paths = TRY(get_paths_for_helper_process("ImageDecoder"sv));
    return launch_image_decoder_process(image_decoder_paths);
}

- (ErrorOr<void>)launchImageDecoder
{
    m_image_decoder_client = TRY(launch_new_image_decoder());

    __weak Application* weak_self = self;

    m_image_decoder_client->on_death = [weak_self]() {
        Application* self = weak_self;
        if (self == nil) {
            return;
        }

        m_image_decoder_client = nullptr;

        if (auto err = [self launchImageDecoder]; err.is_error()) {
            dbgln("Failed to restart image decoder: {}", err.error());
            VERIFY_NOT_REACHED();
        }

        auto num_clients = WebView::WebContentClient::client_count();
        auto new_sockets = m_image_decoder_client->send_sync_but_allow_failure<Messages::ImageDecoderServer::ConnectNewClients>(num_clients);
        if (!new_sockets || new_sockets->sockets().size() == 0) {
            dbgln("Failed to connect {} new clients to ImageDecoder", num_clients);
            VERIFY_NOT_REACHED();
        }

        WebView::WebContentClient::for_each_client([sockets = new_sockets->take_sockets()](WebView::WebContentClient& client) mutable {
            client.async_connect_to_image_decoder(sockets.take_last());
            return IterationDecision::Continue;
        });
    };

    return {};
}

- (ErrorOr<NonnullRefPtr<WebView::WebContentClient>>)launchWebContent:(Ladybird::WebViewBridge&)web_view_bridge
{
    // FIXME: Fail to open the tab, rather than crashing the whole application if this fails
    auto request_server_socket = TRY(connect_new_request_server_client(*m_request_server_client));
    auto image_decoder_socket = TRY(connect_new_image_decoder_client(*m_image_decoder_client));

    auto web_content_paths = TRY(get_paths_for_helper_process("WebContent"sv));
    auto web_content = TRY(launch_web_content_process(web_view_bridge, web_content_paths, move(image_decoder_socket), move(request_server_socket)));

    return web_content;
}

- (ErrorOr<IPC::File>)launchWebWorker
{
    auto web_worker_paths = TRY(get_paths_for_helper_process("WebWorker"sv));
    auto worker_client = TRY(launch_web_worker_process(web_worker_paths, *m_request_server_client));

    return worker_client->clone_transport();
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
