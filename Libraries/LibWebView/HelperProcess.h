/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <LibCompositing/PageId.h>
#include <LibIPC/TransportHandle.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>

#if defined(HAVE_WASM_COMPILER_SERVICE)
#    include <LibWasmCompilerClient/Client.h>
#endif

namespace WebView {

WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(IsPrivate, Compositing::PageId initial_page_id, Web::HTML::CrossProcessId root_navigable_id);

WEBVIEW_API ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::CompositorClient>> launch_compositor_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebWorkerClient>> launch_web_worker_process(Web::HTML::AgentType, IsPrivate, Web::HTML::WorkerAgentId);
WEBVIEW_API ErrorOr<NonnullRefPtr<Requests::RequestControlClient>> launch_request_server_process();
#if defined(HAVE_WASM_COMPILER_SERVICE)
WEBVIEW_API ErrorOr<NonnullRefPtr<WasmCompilerClient::Client>> launch_wasm_compiler_process();
#endif

// The new client uses the cookies of the given session. That must be the session of the process the client is for.
WEBVIEW_API ErrorOr<IPC::TransportHandle> connect_new_request_server_client(BrowsingSession&);
WEBVIEW_API ErrorOr<IPC::TransportHandle> connect_new_image_decoder_client();
#if defined(HAVE_WASM_COMPILER_SERVICE)
WEBVIEW_API ErrorOr<IPC::TransportHandle> connect_new_wasm_compiler_client();
#endif

}
