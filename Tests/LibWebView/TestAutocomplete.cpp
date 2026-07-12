/*
 * Copyright (c) 2026, sideshowbarker <mike@w3.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/AutocompleteService.h>
#include <LibWebView/Settings.h>
#include <stdlib.h>
#include <unistd.h>

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
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestAutocomplete-{}", Core::StandardPaths::tempfile_directory(), getpid());
    VERIFY(setenv("XDG_CONFIG_HOME", test_config_directory.characters(), 1) == 0);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    WebView::Application::settings().set_autocomplete_engine(Optional<StringView> {});
    WebView::Application::autocomplete_service().update_bookmarks({ {
        .url = "https://ladybird.test/"_string,
        .title = "Ladybird test bookmark"_string,
        .folder = {},
        .favicon_base64_png = {},
    } });

    auto saw_bookmark = false;
    auto saw_bookmark_update = false;
    auto requested_bookmark_update = false;
    auto bookmark_update_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        auto query_returned = false;
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            VERIFY(kind == WebView::AutocompleteResultKind::Final);
            saw_bookmark |= suggestions.contains([](auto const& suggestion) {
                return suggestion.text == "https://ladybird.test/"sv;
            });
            saw_bookmark_update |= suggestions.contains([](auto const& suggestion) {
                return suggestion.text == "https://ladybird-updated.test/"sv;
            });

            if (saw_bookmark && !requested_bookmark_update) {
                requested_bookmark_update = true;
                WebView::Application::autocomplete_service().update_bookmarks({ {
                    .url = "https://ladybird-updated.test/"_string,
                    .title = "Updated test bookmark"_string,
                    .folder = {},
                    .favicon_base64_png = {},
                } });
            }

            if (!saw_bookmark_update)
                return;
            VERIFY(query_returned);
            autocomplete.cancel_pending_query();
            bookmark_update_completed = true;
        };

        autocomplete.query_autocomplete_engine(1, "lady"_string);
        query_returned = true;
        Core::EventLoop::current().spin_until([&]() { return bookmark_update_completed; });
    }

    VERIFY(saw_bookmark);
    VERIFY(saw_bookmark_update);

    // A closed loopback port makes the request fail fast and deterministically, with no real network.
    // Its on_finish synchronously delivers the final result. A data: URL cannot be used because the request
    // server's DNS path requires a host.
    WebView::Application::settings().set_autocomplete_engine(WebView::AutocompleteEngine {
        .name = "Test"sv,
        .query_url = "http://127.0.0.1:47919/{}"sv,
    });

    WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
    auto request_completed = false;
    auto query_returned = false;
    autocomplete.on_autocomplete_query_complete = [&](auto, auto const&, WebView::AutocompleteResultKind kind) {
        if (kind != WebView::AutocompleteResultKind::Final)
            return;
        VERIFY(query_returned);
        autocomplete.cancel_pending_query();
        request_completed = true;
    };

    autocomplete.query_autocomplete_engine(2, "test"_string);
    query_returned = true;
    Core::EventLoop::current().spin_until([&]() { return request_completed; });

    outln("PASS: cancel_pending_query during query completion did not crash");
    return 0;
}
