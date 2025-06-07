/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/MachPortServer.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

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

    auto app = Ladybird::Application::create(arguments);
    WebView::platform_init();

    auto mach_port_server = make<WebView::MachPortServer>();
    WebView::set_mach_server_name(mach_port_server->server_port_name());

    mach_port_server->on_receive_child_mach_port = [&](auto pid, auto port) {
        app->set_process_mach_port(pid, move(port));
    };
    mach_port_server->on_receive_backing_stores = [](WebView::MachPortServer::BackingStoresMessage message) {
        if (auto view = WebView::WebContentClient::view_for_pid_and_page_id(message.pid, message.page_id); view.has_value())
            view->did_allocate_iosurface_backing_stores(message.front_backing_store_id, move(message.front_backing_store_port), message.back_backing_store_id, move(message.back_backing_store_port));
    };

    WebView::BrowserProcess browser_process;
    TRY(app->launch_services());

    if (auto const& browser_options = WebView::Application::browser_options(); !browser_options.headless_mode.has_value()) {
        if (browser_options.force_new_process == WebView::ForceNewProcess::No) {
            auto disposition = TRY(browser_process.connect(browser_options.raw_urls, browser_options.new_window));

            if (disposition == WebView::BrowserProcess::ProcessDisposition::ExitProcess) {
                outln("Opening in existing process");
                return 0;
            }
        }

        browser_process.on_new_tab = [&](auto const& raw_urls) {
            open_urls_from_client(raw_urls, WebView::NewWindow::No);
        };

        browser_process.on_new_window = [&](auto const& raw_urls) {
            open_urls_from_client(raw_urls, WebView::NewWindow::Yes);
        };

        auto* delegate = [[ApplicationDelegate alloc] init];
        [NSApp setDelegate:delegate];
    }

    return WebView::Application::the().execute();
}
