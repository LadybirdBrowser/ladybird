/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <Ladybird/DefaultSettings.h>
#include <Ladybird/MachPortServer.h>
#include <Ladybird/Utilities.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/ChromeProcess.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Application/EventLoopImplementation.h>
#import <UI/Tab.h>
#import <UI/TabController.h>

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

    Core::EventLoopManager::install(*new Ladybird::CFEventLoopManager);
    [application setupWebViewApplication:arguments newTabPageURL:Browser::default_new_tab_url];

    platform_init();

    WebView::ChromeProcess chrome_process;

    if (auto const& chrome_options = WebView::Application::chrome_options(); chrome_options.force_new_process == WebView::ForceNewProcess::No) {
        auto disposition = TRY(chrome_process.connect(chrome_options.raw_urls, chrome_options.new_window));

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

    auto mach_port_server = make<Ladybird::MachPortServer>();
    set_mach_server_name(mach_port_server->server_port_name());
    mach_port_server->on_receive_child_mach_port = [&](auto pid, auto port) {
        WebView::Application::the().set_process_mach_port(pid, move(port));
    };
    mach_port_server->on_receive_backing_stores = [](Ladybird::MachPortServer::BackingStoresMessage message) {
        if (auto view = WebView::WebContentClient::view_for_pid_and_page_id(message.pid, message.page_id); view.has_value())
            view->did_allocate_iosurface_backing_stores(message.front_backing_store_id, move(message.front_backing_store_port), message.back_backing_store_id, move(message.back_backing_store_port));
    };

    // FIXME: Create an abstraction to re-spawn the RequestServer and re-hook up its client hooks to each tab on crash
    TRY([application launchRequestServer]);

    TRY([application launchImageDecoder]);

    auto* delegate = [[ApplicationDelegate alloc] init];
    [NSApp setDelegate:delegate];

    return WebView::Application::the().execute();
}
