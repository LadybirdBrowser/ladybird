/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/ChromeProcess.h>
#include <LibWebView/EventLoop/EventLoopImplementationMacOS.h>
#include <LibWebView/MachPortServer.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <UI/DefaultSettings.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static void open_urls_from_client(Vector<URL::URL> const& urls, WebView::NewWindow new_window)
{
    ApplicationDelegate* delegate = [NSApp delegate];
    Tab* tab = new_window == WebView::NewWindow::Yes ? nil : [delegate activeTab];

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

    Core::EventLoopManager::install(*new WebView::EventLoopManagerMacOS);
    auto url = URL::Parser::basic_parse(Browser::default_new_tab_url);
    VERIFY(url.has_value());
    [application setupWebViewApplication:arguments newTabPageURL:url.release_value()];

    WebView::platform_init();

    WebView::ChromeProcess chrome_process;

    if (auto const& browser_options = WebView::Application::browser_options(); browser_options.force_new_process == WebView::ForceNewProcess::No) {
        auto disposition = TRY(chrome_process.connect(browser_options.raw_urls, browser_options.new_window));

        if (disposition == WebView::ChromeProcess::ProcessDisposition::ExitProcess) {
            outln("Opening in existing process");
            return 0;
        }
    }

    chrome_process.on_new_tab = [&](auto const& raw_urls) {
        open_urls_from_client(raw_urls, WebView::NewWindow::No);
    };

    chrome_process.on_new_window = [&](auto const& raw_urls) {
        open_urls_from_client(raw_urls, WebView::NewWindow::Yes);
    };

    auto mach_port_server = make<WebView::MachPortServer>();
    WebView::set_mach_server_name(mach_port_server->server_port_name());

    mach_port_server->on_receive_child_mach_port = [&](auto pid, auto port) {
        WebView::Application::the().set_process_mach_port(pid, move(port));
    };
    mach_port_server->on_receive_backing_stores = [](WebView::MachPortServer::BackingStoresMessage message) {
        if (auto view = WebView::WebContentClient::view_for_pid_and_page_id(message.pid, message.page_id); view.has_value())
            view->did_allocate_iosurface_backing_stores(message.front_backing_store_id, move(message.front_backing_store_port), message.back_backing_store_id, move(message.back_backing_store_port));
    };

    TRY([application launchServices]);

    auto* delegate = [[ApplicationDelegate alloc] init];
    [NSApp setDelegate:delegate];

    return WebView::Application::the().execute();
}
