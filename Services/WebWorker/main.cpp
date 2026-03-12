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
#include <LibIPC/TransportHandle.h>
#include <LibImageDecoderClient/Client.h>
#include <LibMain/Main.h>
#include <LibRequests/RequestClient.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/Plugins/ImageCodecPlugin.h>
#include <LibWebView/Utilities.h>
#include <WebWorker/ConnectionFromClient.h>

#include <openssl/thread.h>

static ErrorOr<void> connect_to_resource_loader(GC::Heap& heap, IPC::TransportHandle const& handle);
static ErrorOr<void> connect_to_image_decoder(IPC::TransportHandle const& handle);

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

    StringView serenity_resource_root;
    StringView worker_type_string;
    Vector<ByteString> certificates;
    bool expose_experimental_interfaces = false;
    bool enable_http_memory_cache = false;
    bool wait_for_debugger = false;
    bool file_origins_are_tuple_origins = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(serenity_resource_root, "Absolute path to directory for serenity resources", "serenity-resource-root", 'r', "serenity-resource-root");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(expose_experimental_interfaces, "Expose experimental IDL interfaces", "expose-experimental-interfaces");
    args_parser.add_option(enable_http_memory_cache, "Enable HTTP cache", "enable-http-memory-cache");
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

    if (enable_http_memory_cache)
        Web::Fetch::Fetching::set_http_memory_cache_enabled(true);

    OPENSSL_TRY(OSSL_set_max_threads(nullptr, Core::System::hardware_concurrency()));

    Web::HTML::UniversalGlobalScopeMixin::set_experimental_interfaces_exposed(expose_experimental_interfaces);

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);

    Web::Platform::FontPlugin::install(*new Web::Platform::FontPlugin(false));

    Web::Bindings::initialize_main_thread_vm(worker_type);

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<WebWorker::ConnectionFromClient>());

    auto& heap = Web::Bindings::main_thread_vm().heap();
    client->on_request_server_connection = [&heap](auto const& handle) {
        if (auto result = connect_to_resource_loader(heap, handle); result.is_error())
            dbgln("Failed to connect to resource loader: {}", result.error());
    };
    client->on_image_decoder_connection = [](auto const& handle) {
        if (auto result = connect_to_image_decoder(handle); result.is_error())
            dbgln("Failed to connect to image decoder: {}", result.error());
    };

    return event_loop.exec();
}

static ErrorOr<void> connect_to_resource_loader(GC::Heap& heap, IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    auto request_client = TRY(try_make_ref_counted<Requests::RequestClient>(move(transport)));
#ifdef AK_OS_WINDOWS
    auto response = request_client->send_sync<Messages::RequestServer::InitTransport>(Core::System::getpid());
    request_client->transport().set_peer_pid(response->peer_pid());
#endif
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
#ifdef AK_OS_WINDOWS
    auto response = new_client->send_sync<Messages::ImageDecoderServer::InitTransport>(Core::System::getpid());
    new_client->transport().set_peer_pid(response->peer_pid());
#endif
    if (Web::Platform::ImageCodecPlugin::is_initialized())
        static_cast<WebView::ImageCodecPlugin&>(Web::Platform::ImageCodecPlugin::the()).set_client(move(new_client));
    else
        Web::Platform::ImageCodecPlugin::install(*new WebView::ImageCodecPlugin(move(new_client)));
    return {};
}
