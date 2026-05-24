/*
 * Copyright (c) 2025, Ladybird Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdServiceBase.h"
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <unistd.h>
#include <LibImageDecoderClient/Client.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Bindings/AgentType.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/Plugins/ImageCodecPlugin.h>
#include <WebWorker/ConnectionFromClient.h>

static ErrorOr<void> connect_to_resource_loader(GC::Heap& heap, IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    auto request_client = TRY(try_make_ref_counted<Requests::RequestClient>(move(transport)));
    if (Web::ResourceLoader::is_initialized())
        Web::ResourceLoader::the().set_client(move(request_client));
    else
        Web::ResourceLoader::initialize(heap, move(request_client));
    return {};
}

static ErrorOr<void> connect_to_image_decoder(IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    auto new_client = TRY(try_make_ref_counted<ImageDecoderClient::Client>(move(transport)));
    if (Web::Platform::ImageCodecPlugin::is_initialized())
        static_cast<WebView::ImageCodecPlugin&>(Web::Platform::ImageCodecPlugin::the()).set_client(move(new_client));
    else
        Web::Platform::ImageCodecPlugin::install(*new WebView::ImageCodecPlugin(move(new_client)));
    return {};
}

ErrorOr<int> service_main(int ipc_socket)
{
    Core::EventLoop event_loop;

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);
    Web::Platform::FontPlugin::install(*new Web::Platform::FontPlugin(false));

    Web::Bindings::initialize_main_thread_vm(Web::Bindings::AgentType::DedicatedWorker);

    auto& heap = Web::Bindings::main_thread_vm().heap();

    auto socket_or_error = Core::LocalSocket::adopt_fd(ipc_socket);
    if (socket_or_error.is_error()) {
        dbgln("WebWorkerService: failed to adopt fd: {}", socket_or_error.error());
        _exit(1);
    }
    auto client = WebWorker::ConnectionFromClient::construct(make<IPC::Transport>(socket_or_error.release_value()));

    client->on_request_server_connection = [&heap](IPC::TransportHandle const& handle) {
        if (auto result = connect_to_resource_loader(heap, handle); result.is_error())
            dbgln("WebWorkerService: failed to connect to resource loader: {}", result.error());
    };
    client->on_image_decoder_connection = [](IPC::TransportHandle const& handle) {
        if (auto result = connect_to_image_decoder(handle); result.is_error())
            dbgln("WebWorkerService: failed to connect to image decoder: {}", result.error());
    };

    // Each WebWorkerService process serves exactly one worker. Once the event loop
    // exits (worker closed), we exit the process so that static state (JS VM, etc.)
    // is fully reset for the next worker — matching desktop WebWorker behaviour.
    _exit(event_loop.exec());
}
