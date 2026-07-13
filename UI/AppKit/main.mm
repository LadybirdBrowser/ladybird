/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/URL.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>
#import <Utilities/ApplicationIcon.h>

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
                                        isPrivate:WebView::IsPrivate::No
                                      activateTab:activate_tab
                                      tabLocation:TabLocation::end()];

        tab = (Tab*)[controller window];
    }
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto app = TRY(Ladybird::Application::create(arguments));
    if (app->should_exit_after_profile_coordination()) {
        outln("Opening in existing process");
        return 0;
    }

    auto& browser_process = app->browser_process();

    if (auto const& browser_options = WebView::Application::browser_options(); !browser_options.headless_mode.has_value()) {
        Ladybird::set_profile_application_icon(WebView::Application::profile());

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
