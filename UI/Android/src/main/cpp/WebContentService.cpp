/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebContentService.h"
#include "LadybirdServiceBase.h"
#include <AK/LexicalPath.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/System.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibImageDecoderClient/Client.h>
#include <LibMedia/Audio/Loader.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Plugins/ImageCodecPlugin.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/Utilities.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageHost.h>

static ErrorOr<NonnullRefPtr<Requests::RequestClient>> bind_request_server_service()
{
    return bind_service<Requests::RequestClient>(&bind_request_server_java);
}

static ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> bind_image_decoder_service()
{
    return bind_service<ImageDecoderClient::Client>(&bind_image_decoder_java);
}

ErrorOr<int> service_main(int ipc_socket)
{
    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);

    auto image_decoder_client = TRY(bind_image_decoder_service());
    Web::Platform::ImageCodecPlugin::install(*new WebView::ImageCodecPlugin(move(image_decoder_client)));

    Web::Bindings::initialize_main_thread_vm(Web::Bindings::AgentType::SimilarOriginWindow);

    auto request_server_client = TRY(bind_request_server_service());
    Web::ResourceLoader::initialize(Web::Bindings::main_thread_vm().heap(), move(request_server_client));

    bool is_test_mode = false;

    Web::HTML::Window::set_internals_object_exposed(is_test_mode);
    Web::Platform::FontPlugin::install(*new Web::Platform::FontPlugin(is_test_mode));

    // Currently site isolation doesn't work on Android since everything is running
    // in the same process. It would require an entire redesign of this port
    // in order to make it work. For now, it's better to just disable it.
    WebView::set_site_isolation_mode(WebView::SiteIsolationMode::Disabled);

    auto webcontent_socket = TRY(Core::LocalSocket::adopt_fd(ipc_socket));
    auto webcontent_client = TRY(WebContent::ConnectionFromClient::try_create(make<IPC::Transport>(move(webcontent_socket))));

    return event_loop.exec();
}

template<typename Client>
ErrorOr<NonnullRefPtr<Client>> bind_service(void (*bind_method)(int))
{
    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds));

    int ui_fd = socket_fds[0];
    int server_fd = socket_fds[1];

    // NOTE: The java object takes ownership of the socket fds
    (*bind_method)(server_fd);

    auto socket = TRY(Core::LocalSocket::adopt_fd(ui_fd));
    TRY(socket->set_blocking(true));

    auto new_client = TRY(try_make_ref_counted<Client>(make<IPC::Transport>(move(socket))));

    return new_client;
}
