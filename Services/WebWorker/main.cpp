/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibCrypto/OpenSSLForward.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/EventLoopPluginSerenity.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Plugins/FontPlugin.h>
#include <LibWebView/Plugins/ImageCodecPlugin.h>
#include <LibWebView/Utilities.h>
#include <WebWorker/ConnectionFromClient.h>

#include <openssl/thread.h>

static ErrorOr<void> initialize_image_decoder(int image_decoder_socket);
static ErrorOr<void> initialize_resource_loader(GC::Heap&, int request_server_socket);

static ErrorOr<Web::Bindings::AgentType> agent_type_from_string(StringView type)
{
    if (type == "dedicated"sv)
        return Web::Bindings::AgentType::DedicatedWorker;
    if (type == "shared"sv)
        return Web::Bindings::AgentType::SharedWorker;
    if (type == "service"sv)
        return Web::Bindings::AgentType::ServiceWorker;

    return Error::from_string_literal("Invalid worker type, must be one of: 'dedicated', 'shared', or 'service'");
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    int request_server_socket { -1 };
    int image_decoder_socket { -1 };
    StringView serenity_resource_root;
    StringView worker_type_string;
    Vector<ByteString> certificates;
    bool wait_for_debugger = false;
    bool file_origins_are_tuple_origins = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(request_server_socket, "File descriptor of the request server socket", "request-server-socket", 's', "request-server-socket");
    args_parser.add_option(image_decoder_socket, "File descriptor of the socket for the ImageDecoder connection", "image-decoder-socket", 'i', "image_decoder_socket");
    args_parser.add_option(serenity_resource_root, "Absolute path to directory for serenity resources", "serenity-resource-root", 'r', "serenity-resource-root");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(worker_type_string, "Type of WebWorker to start (dedicated, shared, or service)", "type", 't', "type");
    args_parser.add_option(file_origins_are_tuple_origins, "Treat file:// URLs as having tuple origins", "tuple-file-origins");

    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (file_origins_are_tuple_origins)
        URL::set_file_scheme_urls_have_tuple_origins();

    auto worker_type = TRY(agent_type_from_string(worker_type_string));

    Core::EventLoop event_loop;

    WebView::platform_init();

    OPENSSL_TRY(OSSL_set_max_threads(nullptr, Core::System::hardware_concurrency()));

    TRY(initialize_image_decoder(image_decoder_socket));

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPluginSerenity);

    Web::Platform::FontPlugin::install(*new WebView::FontPlugin(false));

    Web::Bindings::initialize_main_thread_vm(worker_type);

    TRY(initialize_resource_loader(Web::Bindings::main_thread_vm().heap(), request_server_socket));

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<WebWorker::ConnectionFromClient>());

    return event_loop.exec();
}

static ErrorOr<void> initialize_image_decoder(int image_decoder_socket)
{
#if !defined(AK_OS_WINDOWS)
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");
#else
    static_assert(IsSame<IPC::Transport, IPC::TransportSocketWindows>, "Need to handle other IPC transports here");
#endif
    auto socket = TRY(Core::LocalSocket::adopt_fd(image_decoder_socket));
    TRY(socket->set_blocking(true));

    auto new_client = TRY(try_make_ref_counted<ImageDecoderClient::Client>(make<IPC::Transport>(move(socket))));
#ifdef AK_OS_WINDOWS
    auto response = new_client->send_sync<Messages::ImageDecoderServer::InitTransport>(Core::System::getpid());
    new_client->transport().set_peer_pid(response->peer_pid());
#endif

    Web::Platform::ImageCodecPlugin::install(*new WebView::ImageCodecPlugin(move(new_client)));

    return {};
}

static ErrorOr<void> initialize_resource_loader(GC::Heap& heap, int request_server_socket)
{
#if !defined(AK_OS_WINDOWS)
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");
#else
    static_assert(IsSame<IPC::Transport, IPC::TransportSocketWindows>, "Need to handle other IPC transports here");
#endif
    auto socket = TRY(Core::LocalSocket::adopt_fd(request_server_socket));
    TRY(socket->set_blocking(true));

    auto request_client = TRY(try_make_ref_counted<Requests::RequestClient>(make<IPC::Transport>(move(socket))));
#ifdef AK_OS_WINDOWS
    auto response = request_client->send_sync<Messages::RequestServer::InitTransport>(Core::System::getpid());
    request_client->transport().set_peer_pid(response->peer_pid());
#endif
    Web::ResourceLoader::initialize(heap, move(request_client));

    return {};
}
