/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <LibImageDecoderClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Worker/WebWorkerClient.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(
    WebView::ViewImplementation& view,
    ReadonlySpan<ByteString> candidate_web_content_paths,
    IPC::File image_decoder_socket,
    Optional<IPC::File> request_server_socket = {});

ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process(ReadonlySpan<ByteString> candidate_image_decoder_paths);
ErrorOr<NonnullRefPtr<Web::HTML::WebWorkerClient>> launch_web_worker_process(ReadonlySpan<ByteString> candidate_web_worker_paths, NonnullRefPtr<Requests::RequestClient>);
ErrorOr<NonnullRefPtr<Requests::RequestClient>> launch_request_server_process(ReadonlySpan<ByteString> candidate_request_server_paths, StringView serenity_resource_root);

ErrorOr<IPC::File> connect_new_request_server_client(Requests::RequestClient&);
ErrorOr<IPC::File> connect_new_image_decoder_client(ImageDecoderClient::Client&);
