/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <Ladybird/DefaultSettings.h>
#include <Ladybird/MachPortServer.h>
#include <Ladybird/Types.h>
#include <Ladybird/Utilities.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMain/Main.h>
#include <LibWebView/ChromeProcess.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/Database.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/URL.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Application/EventLoopImplementation.h>
#import <UI/Tab.h>
#import <UI/TabController.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static Vector<URL::URL> sanitize_urls(Vector<ByteString> const& raw_urls)
{
    Vector<URL::URL> sanitized_urls;
    for (auto const& raw_url : raw_urls) {
        if (auto url = WebView::sanitize_url(raw_url); url.has_value())
            sanitized_urls.append(url.release_value());
    }

    if (sanitized_urls.is_empty()) {
        URL::URL new_tab_page_url = Browser::default_new_tab_url;
        sanitized_urls.append(move(new_tab_page_url));
    }

    return sanitized_urls;
}

enum class NewWindow {
    No,
    Yes,
};

static void open_urls_from_client(Vector<ByteString> const& raw_urls, NewWindow new_window)
{
    ApplicationDelegate* delegate = [NSApp delegate];
    Tab* tab = new_window == NewWindow::Yes ? nil : [delegate activeTab];

    auto urls = sanitize_urls(raw_urls);

    for (auto [i, url] : enumerate(urls)) {
        auto activate_tab = i == 0 ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No;

        auto* controller = [delegate createNewTab:url
                                          fromTab:tab
                                      activateTab:activate_tab];

        tab = (Tab*)[controller window];
    }
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Application* application = [Application sharedApplication];

    Core::EventLoopManager::install(*new Ladybird::CFEventLoopManager);
    Core::EventLoop event_loop;

    platform_init();

    Vector<ByteString> raw_urls;
    Vector<ByteString> certificates;
    StringView webdriver_content_ipc_path;
    bool use_gpu_painting = false;
    bool debug_web_content = false;
    bool log_all_js_exceptions = false;
    bool new_window = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("The Ladybird web browser");
    args_parser.add_positional_argument(raw_urls, "URLs to open", "url", Core::ArgsParser::Required::No);
    args_parser.add_option(webdriver_content_ipc_path, "Path to WebDriver IPC for WebContent", "webdriver-content-path", 0, "path", Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown);
    args_parser.add_option(use_gpu_painting, "Enable GPU painting", "enable-gpu-painting");
    args_parser.add_option(debug_web_content, "Wait for debugger to attach to WebContent", "debug-web-content");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(new_window, "Force opening in a new window", "new-window", 'n');
    args_parser.parse(arguments);

    WebView::ChromeProcess chrome_process;
    if (TRY(chrome_process.connect(raw_urls, new_window)) == WebView::ChromeProcess::ProcessDisposition::ExitProcess) {
        outln("Opening in existing process");
        return 0;
    }

    chrome_process.on_new_tab = [&](auto const& raw_urls) {
        open_urls_from_client(raw_urls, NewWindow::No);
    };

    chrome_process.on_new_window = [&](auto const& raw_urls) {
        open_urls_from_client(raw_urls, NewWindow::Yes);
    };

    WebView::ProcessManager::initialize();

    auto mach_port_server = make<Ladybird::MachPortServer>();
    set_mach_server_name(mach_port_server->server_port_name());
    mach_port_server->on_receive_child_mach_port = [](auto pid, auto port) {
        WebView::ProcessManager::the().add_process(pid, move(port));
    };

    auto database = TRY(WebView::Database::create());
    auto cookie_jar = TRY(WebView::CookieJar::create(*database));

    // FIXME: Create an abstraction to re-spawn the RequestServer and re-hook up its client hooks to each tab on crash
    TRY([application launchRequestServer:certificates]);

    StringBuilder command_line_builder;
    command_line_builder.join(' ', arguments.strings);
    Ladybird::WebContentOptions web_content_options {
        .command_line = MUST(command_line_builder.to_string()),
        .executable_path = MUST(String::from_byte_string(MUST(Core::System::current_executable_path()))),
        .enable_gpu_painting = use_gpu_painting ? Ladybird::EnableGPUPainting::Yes : Ladybird::EnableGPUPainting::No,
        .wait_for_debugger = debug_web_content ? Ladybird::WaitForDebugger::Yes : Ladybird::WaitForDebugger::No,
        .log_all_js_exceptions = log_all_js_exceptions ? Ladybird::LogAllJSExceptions::Yes : Ladybird::LogAllJSExceptions::No,
    };

    auto* delegate = [[ApplicationDelegate alloc] init:sanitize_urls(raw_urls)
                                         newTabPageURL:URL::URL { Browser::default_new_tab_url }
                                         withCookieJar:move(cookie_jar)
                                     webContentOptions:web_content_options
                               webdriverContentIPCPath:webdriver_content_ipc_path];

    [NSApp setDelegate:delegate];

    return event_loop.exec();
}
