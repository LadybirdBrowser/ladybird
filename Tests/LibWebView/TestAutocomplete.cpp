/*
 * Copyright (c) 2026, sideshowbarker <mike@w3.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/AutocompleteService.h>
#include <LibWebView/Settings.h>
#include <stdlib.h>

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

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

// Canceling the pending autocomplete query from inside the query-complete callback is exactly what the Qt location bar
// does: Activating a suggestion clears the bar's focus — whose focusOutEvent calls Autocomplete::cancel_pending_query.
// That cancel runs while the request's completion callback is still on the stack — so, tearing the request down freed a
// callback mid-call, and tripped an assert. Regression test for #10278.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestAutocomplete-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
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

    auto saw_adaptive_result = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.record_engagement({
            .input = "docs"_string,
            .destination_kind = WebView::OmniboxDestinationKind::URL,
            .destination = "https://example.com/manual/"_string,
            .was_explicit = true,
        });
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            saw_adaptive_result = suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive;
            });
            autocomplete.cancel_pending_query();
        };
        autocomplete.query_autocomplete_engine(2, "docs"_string);
        Core::EventLoop::current().spin_until([&]() { return saw_adaptive_result; });
    }

    auto private_read_completed = false;
    {
        WebView::Autocomplete private_autocomplete { WebView::IsPrivate::Yes };
        private_autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive
                    && suggestion.text == "https://example.com/manual/"sv;
            }));
            private_autocomplete.cancel_pending_query();
            private_read_completed = true;
        };
        private_autocomplete.query_autocomplete_engine(3, "docs"_string);
        Core::EventLoop::current().spin_until([&]() { return private_read_completed; });
    }

    {
        WebView::Autocomplete private_autocomplete { WebView::IsPrivate::Yes };
        private_autocomplete.record_engagement({
            .input = "privatealias"_string,
            .destination_kind = WebView::OmniboxDestinationKind::URL,
            .destination = "https://private.example/"_string,
            .was_explicit = true,
        });
    }

    auto private_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive;
            }));
            autocomplete.cancel_pending_query();
            private_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(4, "privatealias"_string);
        Core::EventLoop::current().spin_until([&]() { return private_query_completed; });
    }

    WebView::Application::autocomplete_service().update_bookmarks({ {
        .url = "https://github.com/LadybirdBrowser/ladybird"_string,
        .title = "GH"_string,
        .folder = {},
        .favicon_base64_png = {},
    } });
    auto short_bookmark_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().text == "https://github.com/LadybirdBrowser/ladybird"sv);
            VERIFY(suggestions.first().title == "GH"sv);
            VERIFY(suggestions.first().match_class == WebView::AutocompleteMatchClass::ExactTitle);
            VERIFY(!suggestions.first().can_be_automatically_selected);
            VERIFY(!suggestions.first().can_be_inline_completed);
            autocomplete.cancel_pending_query();
            short_bookmark_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(5, "GH"_string);
        Core::EventLoop::current().spin_until([&]() { return short_bookmark_query_completed; });
    }

    WebView::Application::settings().set_search_engine("Google"sv);
    auto whitespace_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.is_verbatim
                    && suggestion.text == "  spaced   query  "sv;
            }));
            autocomplete.cancel_pending_query();
            whitespace_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(6, "  spaced   query  "_string);
        Core::EventLoop::current().spin_until([&]() { return whitespace_query_completed; });
    }

    auto about_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Search;
            }));
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::WebUI
                    && suggestion.text == "about:settings"sv;
            }));
            autocomplete.cancel_pending_query();
            about_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(100, "about:set"_string);
        Core::EventLoop::current().spin_until([&]() { return about_query_completed; });
    }

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

    autocomplete.query_autocomplete_engine(7, "test"_string);
    query_returned = true;
    Core::EventLoop::current().spin_until([&]() { return request_completed; });

    outln("PASS: cancel_pending_query during query completion did not crash");
    return 0;
}
