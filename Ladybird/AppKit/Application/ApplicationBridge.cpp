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
#include <LibProtocol/RequestClient.h>
#include <LibWebView/WebContentClient.h>

namespace Ladybird {

// Unfortunately, the Protocol namespace conflicts hard with a @Protocol interface defined by Objective-C. And the #define
// trick we use for e.g. Duration does not work for Protocol. So here, we make sure that any use of the Protocol namespace
// is limited to .cpp files (i.e. not .h files that an Objective-C file can include).
struct ApplicationBridgeImpl {
    RefPtr<Protocol::RequestClient> request_server_client;
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

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> ApplicationBridge::launch_web_content(WebViewBridge& web_view_bridge)
{
    // FIXME: Fail to open the tab, rather than crashing the whole application if this fails
    auto request_server_socket = TRY(connect_new_request_server_client(*m_impl->request_server_client));

    auto web_content_paths = TRY(get_paths_for_helper_process("WebContent"sv));
    auto web_content = TRY(launch_web_content_process(web_view_bridge, web_content_paths, web_view_bridge.web_content_options(), move(request_server_socket)));

    return web_content;
}

ErrorOr<IPC::File> ApplicationBridge::launch_web_worker()
{
    auto web_worker_paths = TRY(get_paths_for_helper_process("WebWorker"sv));
    auto worker_client = TRY(launch_web_worker_process(web_worker_paths, *m_impl->request_server_client));

    return worker_client->dup_socket();
}

}
