/*
 * Copyright (c) 2026, sideshowbarker <mike@w3.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Settings.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }
};

}

// Canceling the pending autocomplete query from inside the query-complete callback is exactly what the Qt location bar
// does: Activating a suggestion clears the bar's focus — whose focusOutEvent calls Autocomplete::cancel_pending_query.
// That cancel runs while the request's completion callback is still on the stack — so, tearing the request down freed a
// callback mid-call, and tripped an assert. Regression test for #10278.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    // A closed loopback port makes the request fail fast and deterministically (connection refused, no real network) —
    // so, its on_finish runs and synchronously delivers the final result to the callback below. (A data: URL can't be
    // used: it has no host, and the request server's DNS path asserts on a host-less URL.)
    WebView::Application::settings().set_autocomplete_engine(WebView::AutocompleteEngine {
        .name = "Test"sv,
        .query_url = "http://127.0.0.1:47919/{}"sv,
    });

    WebView::Autocomplete autocomplete;

    auto completed = false;
    autocomplete.on_autocomplete_query_complete = [&](auto const&, WebView::AutocompleteResultKind kind) {
        // Only the final result is delivered from within the request's on_finish — which is the re-entrant teardown
        // that previously crashed.
        if (kind != WebView::AutocompleteResultKind::Final)
            return;
        autocomplete.cancel_pending_query();
        completed = true;
    };

    autocomplete.query_autocomplete_engine("test"_string);

    Core::EventLoop::current().spin_until([&]() { return completed; });

    outln("PASS: cancel_pending_query during query completion did not crash");
    return 0;
}
