/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/EventLoopPluginSerenity.h>
#include <LibWeb/WebSockets/WebSocket.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Plugins/FontPlugin.h>
#include <LibWebView/Plugins/ImageCodecPlugin.h>
#include <LibWebView/Utilities.h>
#include <WebWorker/ConnectionFromClient.h>

#if defined(HAVE_QT)
#    include <LibWebView/EventLoop/EventLoopImplementationQt.h>
#    include <QCoreApplication>
#endif

static ErrorOr<void> initialize_image_decoder(int image_decoder_socket);
static ErrorOr<void> initialize_resource_loader(GC::Heap&, int request_server_socket);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    int request_server_socket { -1 };
    int image_decoder_socket { -1 };
    StringView serenity_resource_root;
    Vector<ByteString> certificates;
    bool wait_for_debugger = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(request_server_socket, "File descriptor of the request server socket", "request-server-socket", 's', "request-server-socket");
    args_parser.add_option(image_decoder_socket, "File descriptor of the socket for the ImageDecoder connection", "image-decoder-socket", 'i', "image_decoder_socket");
    args_parser.add_option(serenity_resource_root, "Absolute path to directory for serenity resources", "serenity-resource-root", 'r', "serenity-resource-root");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

#if defined(HAVE_QT)
    QCoreApplication app(arguments.argc, arguments.argv);
    Core::EventLoopManager::install(*new WebView::EventLoopManagerQt);
#endif
    Core::EventLoop event_loop;

    WebView::platform_init();

    TRY(initialize_image_decoder(image_decoder_socket));

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPluginSerenity);

    Web::Platform::FontPlugin::install(*new WebView::FontPlugin(false));

    TRY(Web::Bindings::initialize_main_thread_vm(Web::HTML::EventLoop::Type::Worker));

    TRY(initialize_resource_loader(Web::Bindings::main_thread_vm().heap(), request_server_socket));

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<WebWorker::ConnectionFromClient>());

    return event_loop.exec();
}

static ErrorOr<void> initialize_image_decoder(int image_decoder_socket)
{
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");
    auto socket = TRY(Core::LocalSocket::adopt_fd(image_decoder_socket));
    TRY(socket->set_blocking(true));

    auto new_client = TRY(try_make_ref_counted<ImageDecoderClient::Client>(IPC::Transport(move(socket))));
#ifdef AK_OS_WINDOWS
    auto response = new_client->send_sync<Messages::ImageDecoderServer::InitTransport>(Core::System::getpid());
    new_client->transport().set_peer_pid(response->peer_pid());
#endif

    Web::Platform::ImageCodecPlugin::install(*new WebView::ImageCodecPlugin(move(new_client)));

    return {};
}

static ErrorOr<void> initialize_resource_loader(GC::Heap& heap, int request_server_socket)
{
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");

    auto socket = TRY(Core::LocalSocket::adopt_fd(request_server_socket));
    TRY(socket->set_blocking(true));

    auto request_client = TRY(try_make_ref_counted<Requests::RequestClient>(IPC::Transport(move(socket))));
#ifdef AK_OS_WINDOWS
    auto response = request_client->send_sync<Messages::RequestServer::InitTransport>(Core::System::getpid());
    request_client->transport().set_peer_pid(response->peer_pid());
#endif
    Web::ResourceLoader::initialize(heap, move(request_client));

    return {};
}
