/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Worker/WebWorkerClient.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(
    WebView::ViewImplementation& view,
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket = {});

WEBVIEW_API ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_spare_web_content_process(
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket = {});

WEBVIEW_API ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process();
WEBVIEW_API ErrorOr<NonnullRefPtr<Web::HTML::WebWorkerClient>> launch_web_worker_process(Web::Bindings::AgentType);
WEBVIEW_API ErrorOr<NonnullRefPtr<Requests::RequestClient>> launch_request_server_process();

WEBVIEW_API ErrorOr<IPC::File> connect_new_request_server_client();
WEBVIEW_API ErrorOr<IPC::File> connect_new_image_decoder_client();

}
