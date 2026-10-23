/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibAudio/ClientConnection.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Platform/TaskRole.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <LibCore/Process.h>
#include <LibCore/Resource.h>
#include <LibCore/StatsLog.h>
#include <LibCore/System.h>
#include <LibCore/TimeZone.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/OpenSSLForward.h>
#include <LibGfx/Font/SystemFallbackFonts.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>
#include <LibMain/Main.h>
#include <LibMediaClient/Client.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibUnicode/TimeZone.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Internals/Internals.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/RemoteImageCodecPlugin.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <Services/RendererSandbox.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/WebContentCompositorHost.h>
#include <WebContent/WebDriverConnection.h>

#include <openssl/thread.h>

#include <LibIPC/SingleServer.h>

#if !defined(AK_OS_WINDOWS)
#    include <signal.h>
static void crash_signal_handler(int signo)
{
    char const* name;
    switch (signo) {
    case SIGSEGV:
        name = "SIGSEGV";
        break;
    case SIGBUS:
        name = "SIGBUS";
        break;
    case SIGFPE:
        name = "SIGFPE";
        break;
    case SIGABRT:
        name = "SIGABRT";
        break;
    case SIGILL:
        name = "SIGILL";
        break;
    default:
        name = "unknown";
        break;
    }
    warnln("\n\033[31;1mCRASH\033[0m: Received signal {} ({})", name, signo);
    dump_backtrace(2, 100);
    Core::Process::terminate_immediately(128 + signo);
}

static void install_crash_signal_handlers()
{
    struct sigaction sa;
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
}
#else
#    include <AK/Windows.h>
#    include <signal.h>

static StringView crash_exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION"sv;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION"sv;
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW"sv;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO"sv;
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR"sv;
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT"sv;
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION"sv;
    default:
        return "unknown"sv;
    }
}

// Runs only for exceptions nothing else handles, after the vectored handlers that recover from faults in compiled
// Wasm code have declined them.
static LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS* exception)
{
    auto code = exception->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && exception->ExceptionRecord->NumberParameters >= 2) {
        auto operation = "executing"sv;
        if (exception->ExceptionRecord->ExceptionInformation[0] == 0)
            operation = "reading"sv;
        else if (exception->ExceptionRecord->ExceptionInformation[0] == 1)
            operation = "writing"sv;
        warnln("\n\033[31;1mCRASH\033[0m: Received exception {} ({:#x}) {} {:p}", crash_exception_name(code), code,
            operation, reinterpret_cast<void*>(exception->ExceptionRecord->ExceptionInformation[1]));
    } else {
        warnln("\n\033[31;1mCRASH\033[0m: Received exception {} ({:#x}) at {:p}", crash_exception_name(code), code,
            exception->ExceptionRecord->ExceptionAddress);
    }
    dump_backtrace(2, 100);
    Core::Process::terminate_immediately(static_cast<int>(code));
}

static void crash_abort_handler(int)
{
    warnln("\n\033[31;1mCRASH\033[0m: Received signal SIGABRT ({})", SIGABRT);
    dump_backtrace(2, 100);
    Core::Process::terminate_immediately(128 + SIGABRT);
}

static void install_crash_signal_handlers()
{
    // Leave room on an overflowed stack for the backtrace to be printed.
    ULONG stack_guarantee = 64 * KiB;
    SetThreadStackGuarantee(&stack_guarantee);
    SetUnhandledExceptionFilter(crash_exception_filter);
    (void)signal(SIGABRT, crash_abort_handler);
}
#endif

static ErrorOr<void> connect_to_resource_loader(GC::Heap& heap, IPC::TransportHandle const& handle, Optional<int> client_id);
static ErrorOr<void> connect_to_image_decoder(IPC::TransportHandle const& handle);

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    install_crash_signal_handlers();

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

    WebView::platform_init();

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);

    StringView mach_server_name {};
    Vector<ByteString> certificates;
    int crash_report_fd = -1;
    bool enable_test_mode = false;
    bool expose_experimental_interfaces = false;
    bool expose_internals_object = false;
    bool wait_for_debugger = false;
    bool log_all_js_exceptions = false;
    bool enable_http_memory_cache = false;
    bool collect_garbage_on_every_allocation = false;
    bool is_headless = false;
    bool disable_scrollbar_painting = false;
    bool disable_sandbox = false;
    StringView echo_server_port_string_view {};
    StringView https_echo_server_port_string_view {};
    StringView default_time_zone {};
    bool file_origins_are_tuple_origins = false;

    StringView stats_log_directory;
    Core::ArgsParser args_parser;
    args_parser.add_option(stats_log_directory, "Directory for process statistics (JSON lines)", "stats-log-dir", 0, "path");
    args_parser.add_option(crash_report_fd, "Descriptor for anonymous crash diagnostics", "crash-report-fd", 0, "fd");
    args_parser.add_option(enable_test_mode, "Enable test mode", "test-mode");
    args_parser.add_option(expose_experimental_interfaces, "Expose experimental IDL interfaces", "expose-experimental-interfaces");
    args_parser.add_option(expose_internals_object, "Expose internals object", "expose-internals-object");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(enable_http_memory_cache, "Enable HTTP cache", "enable-http-memory-cache");
    args_parser.add_option(collect_garbage_on_every_allocation, "Collect garbage after every JS heap allocation", "collect-garbage-on-every-allocation");
    args_parser.add_option(disable_scrollbar_painting, "Don't paint horizontal or vertical viewport scrollbars", "disable-scrollbar-painting");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.add_option(echo_server_port_string_view, "Echo server port used in test internals", "echo-server-port", 0, "echo_server_port");
    args_parser.add_option(https_echo_server_port_string_view, "HTTPS echo server port used in test internals", "https-echo-server-port", 0, "https_echo_server_port");
    args_parser.add_option(is_headless, "Report that the browser is running in headless mode", "headless");
    args_parser.add_option(default_time_zone, "Default time zone", "default-time-zone", 0, "time-zone-id");
    args_parser.add_option(file_origins_are_tuple_origins, "Treat file:// URLs as having tuple origins", "tuple-file-origins");
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

    auto stats_log = TRY(Core::StatsLog::create(stats_log_directory, "WebContent"sv));

    if (crash_report_fd >= 0) {
        if (auto result = Core::CrashHandler::initialize(crash_report_fd); result.is_error())
            warnln("Could not install crash report handler: {}", result.error());
    }

    if (wait_for_debugger) {
        Core::Process::wait_for_debugger_and_break();
    }

    if (auto result = Core::Platform::adopt_foreground_application_task_role(); result.is_error())
        warnln("Could not adopt the foreground application task role: {}", result.error());
    // Match the UI process, but let its main thread win a tie, as WebKit does for its WebContent process.
    if (auto result = Core::Platform::set_current_thread_qos(Core::Platform::ThreadQoS::UserInteractive, -1); result.is_error())
        warnln("Could not set main thread QoS: {}", result.error());

    if (!default_time_zone.is_empty()) {
        if (auto result = Core::TimeZone::set_current_time_zone(default_time_zone); result.is_error())
            dbgln("Failed to set default time zone: {}", result.error());
    }

    if (file_origins_are_tuple_origins)
        URL::set_file_scheme_urls_have_tuple_origins();

    WebContent::PageClient::set_is_headless(is_headless);

    if (enable_http_memory_cache)
        Web::Fetch::Fetching::set_http_memory_cache_enabled(true);

    Web::Painting::set_paint_viewport_scrollbars(!disable_scrollbar_painting);

    if (!echo_server_port_string_view.is_empty()) {
        if (auto maybe_echo_server_port = echo_server_port_string_view.to_number<u16>(); maybe_echo_server_port.has_value())
            Web::Internals::Internals::set_echo_server_port(maybe_echo_server_port.value());
        else
            VERIFY_NOT_REACHED();
    }

    if (!https_echo_server_port_string_view.is_empty()) {
        if (auto maybe_https_echo_server_port = https_echo_server_port_string_view.to_number<u16>(); maybe_https_echo_server_port.has_value())
            Web::Internals::Internals::set_https_echo_server_port(maybe_https_echo_server_port.value());
        else
            VERIFY_NOT_REACHED();
    }

    OPENSSL_TRY(OSSL_set_max_threads(nullptr, Core::System::hardware_concurrency()));

    Web::HTML::Window::set_enable_test_mode(enable_test_mode);
    Web::HTML::Window::set_internals_object_exposed(expose_internals_object);
    Web::HTML::WindowOrWorkerGlobalScopeMixin::set_experimental_interfaces_exposed(expose_experimental_interfaces);

    Web::Bindings::initialize_main_thread_vm(Web::HTML::AgentType::SimilarOriginWindow);
    if (stats_log)
        Web::Bindings::main_thread_vm().heap().enable_stats();

    if (collect_garbage_on_every_allocation)
        Web::Bindings::main_thread_vm().heap().set_should_collect_on_every_allocation(true);

    if (log_all_js_exceptions) {
        JS::set_log_all_js_exceptions(true);
    }

    if (!disable_sandbox)
        TRY(RendererSandbox::apply_sandbox(mach_server_name));

    auto webcontent_client = TRY(IPC::take_over_accepted_client_from_system_server<WebContent::ConnectionFromClient>(mach_server_name, enable_test_mode));

    auto& heap = Web::Bindings::main_thread_vm().heap();
    webcontent_client->on_request_server_connection = [&heap](auto const& handle, auto client_id) {
        if (auto result = connect_to_resource_loader(heap, handle, client_id); result.is_error())
            dbgln("Failed to connect to resource loader: {}", result.error());
    };
    webcontent_client->on_image_decoder_connection = [](auto const& handle) {
        if (auto result = connect_to_image_decoder(handle); result.is_error())
            dbgln("Failed to connect to image decoder: {}", result.error());
    };

    MediaClient::Client::set_transport_factory([webcontent_client] {
        return webcontent_client->request_media_server_transport();
    });
    // WebAudio's output plays through the AudioServer; this process never reaches a device itself.
    Audio::ClientConnection::set_transport_factory([webcontent_client] {
        return webcontent_client->request_audio_server_transport();
    });

    if (stats_log) {
        stats_log->start_sampling([&heap](JsonObject& values) {
            heap.add_stats(values);
            Web::HTML::main_thread_event_loop().add_stats(values);
            values.set("fonts.system_fallback_cache_entries"sv, Gfx::system_fallback_font_cache_size());
        });
    }

    return event_loop.exec();
}

ErrorOr<void> connect_to_resource_loader(GC::Heap& heap, IPC::TransportHandle const& handle, Optional<int> client_id)
{
    auto transport = TRY(handle.create_transport());
    // Querying the client ID runs the event loop and could dispatch a request before the client is ready.
    auto request_client = client_id.has_value()
        ? TRY(try_make_ref_counted<Requests::RequestClient>(move(transport), *client_id))
        : TRY(try_make_ref_counted<Requests::RequestClient>(move(transport)));
    if (Web::ResourceLoader::is_initialized())
        Web::ResourceLoader::the().set_client(move(request_client));
    else
        Web::ResourceLoader::initialize(heap, move(request_client));
    return {};
}

ErrorOr<void> connect_to_image_decoder(IPC::TransportHandle const& handle)
{
    auto transport = TRY(handle.create_transport());
    auto new_client = TRY(try_make_ref_counted<ImageDecoderClient::Client>(move(transport)));
    if (Web::Platform::ImageCodecPlugin::is_initialized())
        static_cast<Web::Platform::RemoteImageCodecPlugin&>(Web::Platform::ImageCodecPlugin::the()).set_client(move(new_client));
    else
        Web::Platform::ImageCodecPlugin::install(*new Web::Platform::RemoteImageCodecPlugin(move(new_client)));
    return {};
}
