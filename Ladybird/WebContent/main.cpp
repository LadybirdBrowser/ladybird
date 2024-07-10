/*
 * Copyright (c) 2020-2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <Ladybird/FontPlugin.h>
#include <Ladybird/ImageCodecPlugin.h>
#include <Ladybird/Utilities.h>
#include <LibAudio/Loader.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Process.h>
#include <LibCore/Resource.h>
#include <LibCore/SystemServerTakeover.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibMain/Main.h>
#include <LibProtocol/RequestClient.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Loader/ContentFilter.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/PermissionsPolicy/AutoplayAllowlist.h>
#include <LibWeb/Platform/AudioCodecPluginAgnostic.h>
#include <LibWeb/Platform/EventLoopPluginSerenity.h>
#include <LibWebView/RequestServerAdapter.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/WebDriverConnection.h>

#if defined(HAVE_QT)
#    include <Ladybird/Qt/EventLoopImplementationQt.h>
#    include <Ladybird/Qt/RequestManagerQt.h>
#    include <QCoreApplication>

#    if defined(HAVE_QT_MULTIMEDIA)
#        include <Ladybird/Qt/AudioCodecPluginQt.h>
#    endif
#endif

#if defined(AK_OS_MACOS)
#    include <LibCore/Platform/ProcessStatisticsMach.h>
#endif

static ErrorOr<void> load_content_filters();
static ErrorOr<void> load_autoplay_allowlist();
static ErrorOr<void> initialize_lagom_networking(int request_server_socket);
static ErrorOr<void> initialize_image_decoder(int image_decoder_socket);
static ErrorOr<void> reinitialize_image_decoder(IPC::File const& image_decoder_socket);

namespace JS {
extern bool g_log_all_js_exceptions;
}

namespace Web::WebIDL {
extern bool g_enable_idl_tracing;
}

namespace Web::Fetch::Fetching {
extern bool g_http_cache_enabled;
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

#if defined(HAVE_QT)
    QCoreApplication app(arguments.argc, arguments.argv);

    Core::EventLoopManager::install(*new Ladybird::EventLoopManagerQt);
#endif
    Core::EventLoop event_loop;

    platform_init();

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPluginSerenity);

    Web::Platform::AudioCodecPlugin::install_creation_hook([](auto loader) {
#if defined(HAVE_QT_MULTIMEDIA)
        return Ladybird::AudioCodecPluginQt::create(move(loader));
#elif defined(AK_OS_MACOS) || defined(HAVE_PULSEAUDIO)
        return Web::Platform::AudioCodecPluginAgnostic::create(move(loader));
#else
        (void)loader;
        return Error::from_string_literal("Don't know how to initialize audio in this configuration!");
#endif
    });

    StringView command_line {};
    StringView executable_path {};
    StringView mach_server_name {};
    Vector<ByteString> certificates;
    int request_server_socket { -1 };
    int image_decoder_socket { -1 };
    bool is_layout_test_mode = false;
    bool expose_internals_object = false;
    bool use_lagom_networking = false;
    bool use_skia_painter = false;
    bool wait_for_debugger = false;
    bool log_all_js_exceptions = false;
    bool enable_idl_tracing = false;
    bool enable_http_cache = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(command_line, "Chrome process command line", "command-line", 0, "command_line");
    args_parser.add_option(executable_path, "Chrome process executable path", "executable-path", 0, "executable_path");
    args_parser.add_option(request_server_socket, "File descriptor of the socket for the RequestServer connection", "request-server-socket", 'r', "request_server_socket");
    args_parser.add_option(image_decoder_socket, "File descriptor of the socket for the ImageDecoder connection", "image-decoder-socket", 'i', "image_decoder_socket");
    args_parser.add_option(is_layout_test_mode, "Is layout test mode", "layout-test-mode");
    args_parser.add_option(expose_internals_object, "Expose internals object", "expose-internals-object");
    args_parser.add_option(use_lagom_networking, "Enable Lagom servers for networking", "use-lagom-networking");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(use_skia_painter, "Enable Skia painter", "use-skia-painting");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(enable_idl_tracing, "Enable IDL tracing", "enable-idl-tracing");
    args_parser.add_option(enable_http_cache, "Enable HTTP cache", "enable-http-cache");

    args_parser.parse(arguments);

    if (wait_for_debugger) {
        Core::Process::wait_for_debugger_and_break();
    }

    // Layout test mode implies internals object is exposed
    if (is_layout_test_mode)
        expose_internals_object = true;

    Web::set_chrome_process_command_line(command_line);
    Web::set_chrome_process_executable_path(executable_path);

    if (use_skia_painter) {
        WebContent::PageClient::set_use_skia_painter();
    }

    if (enable_http_cache) {
        Web::Fetch::Fetching::g_http_cache_enabled = true;
    }

#if defined(AK_OS_MACOS)
    if (!mach_server_name.is_empty()) {
        auto server_port = Core::Platform::register_with_mach_server(mach_server_name);
        WebContent::BackingStoreManager::set_browser_mach_port(move(server_port));
    }
#endif

#if defined(HAVE_QT)
    if (!use_lagom_networking)
        Web::ResourceLoader::initialize(Ladybird::RequestManagerQt::create(certificates));
    else
#endif
        TRY(initialize_lagom_networking(request_server_socket));

    TRY(initialize_image_decoder(image_decoder_socket));

    Web::HTML::Window::set_internals_object_exposed(expose_internals_object);

    Web::Platform::FontPlugin::install(*new Ladybird::FontPlugin(is_layout_test_mode));

    TRY(Web::Bindings::initialize_main_thread_vm(Web::HTML::EventLoop::Type::Window));

    if (log_all_js_exceptions) {
        JS::g_log_all_js_exceptions = true;
    }

    if (enable_idl_tracing) {
        Web::WebIDL::g_enable_idl_tracing = true;
    }

    auto maybe_content_filter_error = load_content_filters();
    if (maybe_content_filter_error.is_error())
        dbgln("Failed to load content filters: {}", maybe_content_filter_error.error());

    auto maybe_autoplay_allowlist_error = load_autoplay_allowlist();
    if (maybe_autoplay_allowlist_error.is_error())
        dbgln("Failed to load autoplay allowlist: {}", maybe_autoplay_allowlist_error.error());

    auto webcontent_socket = TRY(Core::take_over_socket_from_system_server("WebContent"sv));
    auto webcontent_client = TRY(WebContent::ConnectionFromClient::try_create(move(webcontent_socket)));

    webcontent_client->on_image_decoder_connection = [&](auto& socket_file) {
        auto maybe_error = reinitialize_image_decoder(socket_file);
        if (maybe_error.is_error())
            dbgln("Failed to reinitialize image decoder: {}", maybe_error.error());
    };

    return event_loop.exec();
}

static ErrorOr<void> load_content_filters()
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(4096));

    auto resource = TRY(Core::Resource::load_from_uri("resource://ladybird/BrowserContentFilters.txt"sv));
    auto ad_filter_list = TRY(InputBufferedSeekable<FixedMemoryStream>::create(make<FixedMemoryStream>(resource->data())));

    Vector<String> patterns;

    while (TRY(ad_filter_list->can_read_line())) {
        auto line = TRY(ad_filter_list->read_line(buffer));
        if (line.is_empty())
            continue;

        auto pattern = TRY(String::from_utf8(line));
        TRY(patterns.try_append(move(pattern)));
    }

    auto& content_filter = Web::ContentFilter::the();
    TRY(content_filter.set_patterns(patterns));

    return {};
}

static ErrorOr<void> load_autoplay_allowlist()
{
    auto buffer = TRY(ByteBuffer::create_uninitialized(4096));

    auto resource = TRY(Core::Resource::load_from_uri("resource://ladybird/BrowserAutoplayAllowlist.txt"sv));
    auto allowlist = TRY(InputBufferedSeekable<FixedMemoryStream>::create(make<FixedMemoryStream>(resource->data())));

    Vector<String> origins;

    while (TRY(allowlist->can_read_line())) {
        auto line = TRY(allowlist->read_line(buffer));
        if (line.is_empty())
            continue;

        auto domain = TRY(String::from_utf8(line));
        TRY(origins.try_append(move(domain)));
    }

    auto& autoplay_allowlist = Web::PermissionsPolicy::AutoplayAllowlist::the();
    TRY(autoplay_allowlist.enable_for_origins(origins));

    return {};
}

ErrorOr<void> initialize_lagom_networking(int request_server_socket)
{
    auto socket = TRY(Core::LocalSocket::adopt_fd(request_server_socket));
    TRY(socket->set_blocking(true));

    auto new_client = TRY(try_make_ref_counted<Protocol::RequestClient>(move(socket)));

    Web::ResourceLoader::initialize(TRY(WebView::RequestServerAdapter::try_create(move(new_client))));
    return {};
}

ErrorOr<void> initialize_image_decoder(int image_decoder_socket)
{
    auto socket = TRY(Core::LocalSocket::adopt_fd(image_decoder_socket));
    TRY(socket->set_blocking(true));

    auto new_client = TRY(try_make_ref_counted<ImageDecoderClient::Client>(move(socket)));

    Web::Platform::ImageCodecPlugin::install(*new Ladybird::ImageCodecPlugin(move(new_client)));

    return {};
}

ErrorOr<void> reinitialize_image_decoder(IPC::File const& image_decoder_socket)
{
    auto socket = TRY(Core::LocalSocket::adopt_fd(image_decoder_socket.take_fd()));
    TRY(socket->set_blocking(true));

    auto new_client = TRY(try_make_ref_counted<ImageDecoderClient::Client>(move(socket)));

    static_cast<Ladybird::ImageCodecPlugin&>(Web::Platform::ImageCodecPlugin::the()).set_client(move(new_client));

    return {};
}
