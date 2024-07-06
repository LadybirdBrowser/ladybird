/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <Application/ApplicationBridge.h>
#include <Ladybird/AppKit/UI/LadybirdWebViewBridge.h>
#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibImageDecoderClient/Client.h>
#include <LibProtocol/RequestClient.h>
#include <LibWebView/WebContentClient.h>

namespace Ladybird {

// Unfortunately, the Protocol namespace conflicts hard with a @Protocol interface defined by Objective-C. And the #define
// trick we use for e.g. Duration does not work for Protocol. So here, we make sure that any use of the Protocol namespace
// is limited to .cpp files (i.e. not .h files that an Objective-C file can include).
struct ApplicationBridgeImpl {
    RefPtr<Protocol::RequestClient> request_server_client;
    RefPtr<ImageDecoderClient::Client> image_decoder_client;
};

ApplicationBridge::ApplicationBridge()
    : m_impl(make<ApplicationBridgeImpl>())
{
}

ApplicationBridge::~ApplicationBridge() = default;

ErrorOr<void> ApplicationBridge::launch_request_server(Vector<ByteString> const& certificates)
{
    auto request_server_paths = TRY(get_paths_for_helper_process("RequestServer"sv));
    auto protocol_client = TRY(launch_request_server_process(request_server_paths, s_serenity_resource_root, certificates));

    m_impl->request_server_client = move(protocol_client);
    return {};
}

static ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_new_image_decoder()
{
    auto image_decoder_paths = TRY(get_paths_for_helper_process("ImageDecoder"sv));
    return launch_image_decoder_process(image_decoder_paths);
}

ErrorOr<void> ApplicationBridge::launch_image_decoder()
{
    m_impl->image_decoder_client = TRY(launch_new_image_decoder());

    m_impl->image_decoder_client->on_death = [this] {
        m_impl->image_decoder_client = nullptr;
        if (auto err = this->launch_image_decoder(); err.is_error()) {
            dbgln("Failed to restart image decoder: {}", err.error());
            VERIFY_NOT_REACHED();
        }

        auto num_clients = WebView::WebContentClient::client_count();
        auto new_sockets = m_impl->image_decoder_client->send_sync_but_allow_failure<Messages::ImageDecoderServer::ConnectNewClients>(num_clients);
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

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> ApplicationBridge::launch_web_content(WebViewBridge& web_view_bridge)
{
    // FIXME: Fail to open the tab, rather than crashing the whole application if this fails
    auto request_server_socket = TRY(connect_new_request_server_client(*m_impl->request_server_client));
    auto image_decoder_socket = TRY(connect_new_image_decoder_client(*m_impl->image_decoder_client));

    auto web_content_paths = TRY(get_paths_for_helper_process("WebContent"sv));
    auto web_content = TRY(launch_web_content_process(web_view_bridge, web_content_paths, web_view_bridge.web_content_options(), move(image_decoder_socket), move(request_server_socket)));

    return web_content;
}

ErrorOr<IPC::File> ApplicationBridge::launch_web_worker()
{
    auto web_worker_paths = TRY(get_paths_for_helper_process("WebWorker"sv));
    auto worker_client = TRY(launch_web_worker_process(web_worker_paths, m_impl->request_server_client));

    return worker_client->dup_socket();
}

}
