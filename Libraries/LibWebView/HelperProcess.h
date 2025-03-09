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
#include <LibWeb/Worker/WebWorkerClient.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(
    WebView::ViewImplementation& view,
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket = {});

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_spare_web_content_process(
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket = {});

ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process();
ErrorOr<NonnullRefPtr<Web::HTML::WebWorkerClient>> launch_web_worker_process();
ErrorOr<NonnullRefPtr<Requests::RequestClient>> launch_request_server_process();

ErrorOr<IPC::File> connect_new_request_server_client();
ErrorOr<IPC::File> connect_new_image_decoder_client();

}
