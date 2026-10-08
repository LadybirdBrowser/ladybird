/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <LibIPC/TransportHandle.h>
#include <LibMediaClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>
#include <RequestServer/SiteBinding.h>

#if defined(HAVE_WASM_COMPILER_SERVICE)
#    include <LibWasmCompilerClient/Client.h>
#endif

namespace WebView {

WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(IsPrivate, Web::PageId initial_page_id, Web::HTML::CrossProcessId root_navigable_id);

WEBVIEW_API ErrorOr<NonnullRefPtr<MediaClient::Client>> launch_media_server_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::CompositorClient>> launch_compositor_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebWorkerClient>> launch_web_worker_process(Web::HTML::AgentType, IsPrivate, Web::HTML::WorkerAgentId);
WEBVIEW_API ErrorOr<NonnullRefPtr<Requests::RequestControlClient>> launch_request_server_process(ByteString const& cache_path, Optional<Utf16String> const& site, HTTPDiskCacheMode);
WEBVIEW_API void apply_dns_settings(Requests::RequestControlClient&);
#if defined(HAVE_WASM_COMPILER_SERVICE)
WEBVIEW_API ErrorOr<NonnullRefPtr<WasmCompilerClient::Client>> launch_wasm_compiler_process();
#endif

struct ImageDecoderConnection {
    IPC::TransportHandle handle;
    pid_t pid { -1 };
};

// Launches an ImageDecoder that serves a single renderer or worker, which talks to it over the returned handle.
WEBVIEW_API ErrorOr<ImageDecoderConnection> launch_image_decoder_process();

// Hands the client its ImageDecoder. If that decoder dies while the client is still connected, the client gets a new one.
WEBVIEW_API void connect_to_image_decoder(WebContentClient&, ImageDecoderConnection);
WEBVIEW_API void connect_to_image_decoder(WebWorkerClient&, ImageDecoderConnection);

struct RequestServerClientConnection {
    IPC::TransportHandle handle;
    int client_id { -1 };
};

// A new client of the session's RequestServer with no site. The client uses the cookies of the given session, which
// must be the session of the process the client is for.
WEBVIEW_API ErrorOr<RequestServerClientConnection> connect_new_request_server_client(BrowsingSession&, RequestServer::SiteBinding);
// Launches the MediaServer for a renderer if it has none, keeping its controller connection in the given slot, and
// connects a new client to it.
WEBVIEW_API ErrorOr<IPC::TransportHandle> connect_new_media_server_client(RefPtr<MediaClient::Client>& controller);
#if defined(HAVE_WASM_COMPILER_SERVICE)
WEBVIEW_API ErrorOr<IPC::TransportHandle> connect_new_wasm_compiler_client();
#endif

}
