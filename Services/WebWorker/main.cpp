/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/TaskRole.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <LibCore/Process.h>
#include <LibCore/StatsLog.h>
#include <LibCore/System.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/OpenSSLForward.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/SingleServer.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibImageDecoderClient/Client.h>
#include <LibMain/Main.h>
#include <LibMediaClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/RemoteImageCodecPlugin.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <Services/RendererSandbox.h>
#include <WebWorker/ConnectionFromClient.h>

#include <openssl/thread.h>

static ErrorOr<void> connect_to_resource_loader(GC::Heap& heap, IPC::TransportHandle const& handle, Optional<int> client_id, Optional<Utf16String> const& top_level_site);
static ErrorOr<void> connect_to_image_decoder(IPC::TransportHandle const& handle);

static ErrorOr<Web::HTML::AgentType> agent_type_from_string(StringView type)
{
    if (type == "dedicated"sv)
        return Web::HTML::AgentType::DedicatedWorker;
    if (type == "shared"sv)
        return Web::HTML::AgentType::SharedWorker;
    if (type == "service"sv)
        return Web::HTML::AgentType::ServiceWorker;

    return Error::from_string_literal("Invalid worker type, must be one of: 'dedicated', 'shared', or 'service'");
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    StringView serenity_resource_root;
    StringView worker_type_string;
    StringView mach_server_name;
    Vector<ByteString> certificates;
    bool expose_experimental_interfaces = false;
    bool enable_http_memory_cache = false;
    bool wait_for_debugger = false;
    bool file_origins_are_tuple_origins = false;
    bool disable_sandbox = false;
    bool enable_test_mode = false;

    int crash_report_fd = -1;
    StringView stats_log_directory;
    Core::ArgsParser args_parser;
    args_parser.add_option(stats_log_directory, "Directory for process statistics (JSON lines)", "stats-log-dir", 0, "path");
    args_parser.add_option(crash_report_fd, "Descriptor for anonymous crash diagnostics", "crash-report-fd", 0, "fd");
    args_parser.add_option(serenity_resource_root, "Absolute path to directory for serenity resources", "serenity-resource-root", 'r', "serenity-resource-root");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(expose_experimental_interfaces, "Expose experimental IDL interfaces", "expose-experimental-interfaces");
    args_parser.add_option(enable_http_memory_cache, "Enable HTTP cache", "enable-http-memory-cache");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(worker_type_string, "Type of WebWorker to start (dedicated, shared, or service)", "type", 't', "type");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(file_origins_are_tuple_origins, "Treat file:// URLs as having tuple origins", "tuple-file-origins");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.add_option(enable_test_mode, "Enable test mode", "test-mode");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "URL of a PAC file (not supported yet, requests fail)",
        .long_name = "proxy-autoconfig-url",
        .value_name = "url",
        .accept_value = [&](StringView value) {
            auto url = URL::Parser::basic_parse(value);
            if (!url.has_value())
                return false;
            Web::ResourceLoader::set_proxy_autoconfig_url(url.release_value());
            return true;
        },
    });

    args_parser.parse(arguments);

    auto stats_log = TRY(Core::StatsLog::create(stats_log_directory, "WebWorker"sv));

    if (crash_report_fd >= 0) {
        if (auto result = Core::CrashHandler::initialize(crash_report_fd); result.is_error())
            warnln("Could not install crash report handler: {}", result.error());
    }

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (auto result = Core::Platform::adopt_foreground_application_task_role(); result.is_error())
        warnln("Could not adopt the foreground application task role: {}", result.error());
    if (auto result = Core::Platform::set_current_thread_qos(Core::Platform::ThreadQoS::UserInitiated); result.is_error())
        warnln("Could not set main thread QoS: {}", result.error());

    if (file_origins_are_tuple_origins)
        URL::set_file_scheme_urls_have_tuple_origins();

    auto worker_type = TRY(agent_type_from_string(worker_type_string));

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

    WebView::platform_init();

    if (enable_http_memory_cache)
        Web::Fetch::Fetching::set_http_memory_cache_enabled(true);

    OPENSSL_TRY(OSSL_set_max_threads(nullptr, Core::System::hardware_concurrency()));

    Web::HTML::WindowOrWorkerGlobalScopeMixin::set_experimental_interfaces_exposed(expose_experimental_interfaces);

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);

    Web::Bindings::initialize_main_thread_vm(worker_type);
    if (stats_log)
        Web::Bindings::main_thread_vm().heap().enable_stats();

    if (!disable_sandbox)
        TRY(RendererSandbox::apply_sandbox(mach_server_name));

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<WebWorker::ConnectionFromClient>(mach_server_name, enable_test_mode));

    auto& heap = Web::Bindings::main_thread_vm().heap();
    client->on_request_server_connection = [&heap](auto const& handle, auto client_id, auto const& top_level_site) {
        if (auto result = connect_to_resource_loader(heap, handle, client_id, top_level_site); result.is_error())
            dbgln("Failed to connect to resource loader: {}", result.error());
    };
    client->on_image_decoder_connection = [](auto const& handle) {
        if (auto result = connect_to_image_decoder(handle); result.is_error())
            dbgln("Failed to connect to image decoder: {}", result.error());
    };

    MediaClient::Client::set_transport_factory([client] {
        return client->request_media_server_transport();
    });

    if (stats_log)
        stats_log->start_sampling([&heap](JsonObject& values) { heap.add_stats(values); });

    return event_loop.exec();
}

static ErrorOr<void> connect_to_resource_loader(GC::Heap& heap, IPC::TransportHandle const& handle, Optional<int> client_id, Optional<Utf16String> const& top_level_site)
{
    auto transport = TRY(handle.create_transport());
    // Querying the client ID runs the event loop and could dispatch a request before the client is ready.
    auto request_client = client_id.has_value()
        ? TRY(try_make_ref_counted<Requests::RequestClient>(move(transport), *client_id))
        : TRY(try_make_ref_counted<Requests::RequestClient>(move(transport)));
    if (!Web::ResourceLoader::is_initialized())
        Web::ResourceLoader::initialize(heap);
    Web::ResourceLoader::the().set_client(top_level_site, move(request_client));
    return {};
}

static ErrorOr<void> connect_to_image_decoder(IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    auto new_client = TRY(try_make_ref_counted<ImageDecoderClient::Client>(move(transport)));
    if (Web::Platform::ImageCodecPlugin::is_initialized())
        static_cast<Web::Platform::RemoteImageCodecPlugin&>(Web::Platform::ImageCodecPlugin::the()).set_client(move(new_client));
    else
        Web::Platform::ImageCodecPlugin::install(*new Web::Platform::RemoteImageCodecPlugin(move(new_client)));
    return {};
}
