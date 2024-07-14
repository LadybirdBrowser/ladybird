/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Application.h"
#include "BrowserWindow.h"
#include "EventLoopImplementationQt.h"
#include "Settings.h"
#include "WebContentView.h"
#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/ChromeProcess.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/Database.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/URL.h>

#if defined(AK_OS_MACOS)
#    include <Ladybird/MachPortServer.h>
#endif

namespace Ladybird {

// FIXME: Find a place to put this declaration (and other helper functions).
bool is_using_dark_system_theme(QWidget&);
bool is_using_dark_system_theme(QWidget& widget)
{
    // FIXME: Qt does not provide any method to query if the system is using a dark theme. We will have to implement
    //        platform-specific methods if we wish to have better detection. For now, this inspects if Qt is using a
    //        dark color for widget backgrounds using Rec. 709 luma coefficients.
    //        https://en.wikipedia.org/wiki/Rec._709#Luma_coefficients

    auto color = widget.palette().color(widget.backgroundRole());
    auto luma = 0.2126f * color.redF() + 0.7152f * color.greenF() + 0.0722f * color.blueF();

    return luma <= 0.5f;
}

}

static ErrorOr<void> handle_attached_debugger()
{
#ifdef AK_OS_LINUX
    // Let's ignore SIGINT if we're being debugged because GDB
    // incorrectly forwards the signal to us even when it's set to
    // "nopass". See https://sourceware.org/bugzilla/show_bug.cgi?id=9425
    // for details.
    if (TRY(Core::Process::is_being_debugged())) {
        dbgln("Debugger is attached, ignoring SIGINT");
        TRY(Core::System::signal(SIGINT, SIG_IGN));
    }
#endif
    return {};
}

static Vector<URL::URL> sanitize_urls(Vector<ByteString> const& raw_urls)
{
    Vector<URL::URL> sanitized_urls;
    for (auto const& raw_url : raw_urls) {
        if (auto url = WebView::sanitize_url(raw_url); url.has_value())
            sanitized_urls.append(url.release_value());
    }
    return sanitized_urls;
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Ladybird::Application app(arguments.argc, arguments.argv);

    Core::EventLoopManager::install(*new Ladybird::EventLoopManagerQt);
    WebView::Application webview_app(arguments.argc, arguments.argv);
    static_cast<Ladybird::EventLoopImplementationQt&>(Core::EventLoop::current().impl()).set_main_loop();

    TRY(handle_attached_debugger());

    platform_init();

    Vector<ByteString> raw_urls;
    StringView webdriver_content_ipc_path;
    Vector<ByteString> certificates;
    bool enable_callgrind_profiling = false;
    bool disable_sql_database = false;
    bool enable_qt_networking = false;
    bool expose_internals_object = false;
    bool use_skia_painting = false;
    bool debug_web_content = false;
    bool log_all_js_exceptions = false;
    bool enable_idl_tracing = false;
    bool enable_http_cache = false;
    bool new_window = false;
    bool force_new_process = false;
    bool allow_popups = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("The Ladybird web browser :^)");
    args_parser.add_positional_argument(raw_urls, "URLs to open", "url", Core::ArgsParser::Required::No);
    args_parser.add_option(webdriver_content_ipc_path, "Path to WebDriver IPC for WebContent", "webdriver-content-path", 0, "path", Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown);
    args_parser.add_option(enable_callgrind_profiling, "Enable Callgrind profiling", "enable-callgrind-profiling", 'P');
    args_parser.add_option(disable_sql_database, "Disable SQL database", "disable-sql-database");
    args_parser.add_option(enable_qt_networking, "Enable Qt as the backend networking service", "enable-qt-networking");
    args_parser.add_option(use_skia_painting, "Enable Skia painting", "enable-skia-painting");
    args_parser.add_option(debug_web_content, "Wait for debugger to attach to WebContent", "debug-web-content");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(enable_idl_tracing, "Enable IDL tracing", "enable-idl-tracing");
    args_parser.add_option(enable_http_cache, "Enable HTTP cache", "enable-http-cache");
    args_parser.add_option(expose_internals_object, "Expose internals object", "expose-internals-object");
    args_parser.add_option(new_window, "Force opening in a new window", "new-window", 'n');
    args_parser.add_option(force_new_process, "Force creation of new browser/chrome process", "force-new-process");
    args_parser.add_option(allow_popups, "Disable popup blocking by default", "allow-popups");
    args_parser.parse(arguments);

    WebView::ChromeProcess chrome_process;
    if (!force_new_process && TRY(chrome_process.connect(raw_urls, new_window)) == WebView::ChromeProcess::ProcessDisposition::ExitProcess) {
        outln("Opening in existing process");
        return 0;
    }

    chrome_process.on_new_tab = [&](auto const& raw_urls) {
        auto& window = app.active_window();
        auto urls = sanitize_urls(raw_urls);
        for (size_t i = 0; i < urls.size(); ++i) {
            window.new_tab_from_url(urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No);
        }
        window.show();
        window.activateWindow();
        window.raise();
    };

    app.on_open_file = [&](auto file_url) {
        auto& window = app.active_window();
        window.view().load(file_url);
    };

#if defined(AK_OS_MACOS)
    auto mach_port_server = make<Ladybird::MachPortServer>();
    set_mach_server_name(mach_port_server->server_port_name());
    mach_port_server->on_receive_child_mach_port = [&webview_app](auto pid, auto port) {
        webview_app.set_process_mach_port(pid, move(port));
    };
    mach_port_server->on_receive_backing_stores = [](Ladybird::MachPortServer::BackingStoresMessage message) {
        if (auto view = WebView::WebContentClient::view_for_pid_and_page_id(message.pid, message.page_id); view.has_value())
            view->did_allocate_iosurface_backing_stores(message.front_backing_store_id, move(message.front_backing_store_port), message.back_backing_store_id, move(message.back_backing_store_port));
    };
#endif

    copy_default_config_files(Ladybird::Settings::the()->directory());

    RefPtr<WebView::Database> database;
    if (!disable_sql_database)
        database = TRY(WebView::Database::create());

    auto cookie_jar = database ? TRY(WebView::CookieJar::create(*database)) : WebView::CookieJar::create();

    // FIXME: Create an abstraction to re-spawn the RequestServer and re-hook up its client hooks to each tab on crash
    if (!enable_qt_networking) {
        auto request_server_paths = TRY(get_paths_for_helper_process("RequestServer"sv));
        auto protocol_client = TRY(launch_request_server_process(request_server_paths, s_serenity_resource_root, certificates));
        app.request_server_client = move(protocol_client);
    }

    TRY(app.initialize_image_decoder());

    StringBuilder command_line_builder;
    command_line_builder.join(' ', arguments.strings);
    Ladybird::WebContentOptions web_content_options {
        .command_line = MUST(command_line_builder.to_string()),
        .executable_path = MUST(String::from_byte_string(MUST(Core::System::current_executable_path()))),
        .config_path = Ladybird::Settings::the()->directory(),
        .enable_callgrind_profiling = enable_callgrind_profiling ? Ladybird::EnableCallgrindProfiling::Yes : Ladybird::EnableCallgrindProfiling::No,
        .enable_skia_painting = use_skia_painting ? Ladybird::EnableSkiaPainting::Yes : Ladybird::EnableSkiaPainting::No,
        .use_lagom_networking = enable_qt_networking ? Ladybird::UseLagomNetworking::No : Ladybird::UseLagomNetworking::Yes,
        .wait_for_debugger = debug_web_content ? Ladybird::WaitForDebugger::Yes : Ladybird::WaitForDebugger::No,
        .log_all_js_exceptions = log_all_js_exceptions ? Ladybird::LogAllJSExceptions::Yes : Ladybird::LogAllJSExceptions::No,
        .enable_idl_tracing = enable_idl_tracing ? Ladybird::EnableIDLTracing::Yes : Ladybird::EnableIDLTracing::No,
        .enable_http_cache = enable_http_cache ? Ladybird::EnableHTTPCache::Yes : Ladybird::EnableHTTPCache::No,
        .expose_internals_object = expose_internals_object ? Ladybird::ExposeInternalsObject::Yes : Ladybird::ExposeInternalsObject::No,
    };

    chrome_process.on_new_window = [&](auto const& urls) {
        app.new_window(sanitize_urls(urls), *cookie_jar, web_content_options, webdriver_content_ipc_path, allow_popups);
    };

    auto& window = app.new_window(sanitize_urls(raw_urls), *cookie_jar, web_content_options, webdriver_content_ipc_path, allow_popups);
    window.setWindowTitle("Ladybird");

    if (Ladybird::Settings::the()->is_maximized()) {
        window.showMaximized();
    } else {
        auto last_position = Ladybird::Settings::the()->last_position();
        if (last_position.has_value())
            window.move(last_position.value());
        window.resize(Ladybird::Settings::the()->last_size());
    }

    window.show();

    return webview_app.exec();
}
