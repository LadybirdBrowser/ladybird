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

class TestExternalURLHandler final : public WebView::ExternalURLHandler {
public:
    TestExternalURLHandler()
        : WebView::ExternalURLHandler("Test application"_string, false)
    {
    }

    virtual void launch(URL::URL const&, LaunchCallback callback) const override
    {
        callback(true);
    }
};

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

    virtual void resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const override
    {
        m_pending_external_url_handler_queries.append({ url, move(callback) });
    }

    size_t pending_external_url_handler_query_count() const { return m_pending_external_url_handler_queries.size(); }

    StringView pending_external_url_handler_query_scheme(size_t index) const
    {
        return m_pending_external_url_handler_queries[index].url.scheme();
    }

    void complete_next_external_url_handler_query(bool has_handler)
    {
        auto query = m_pending_external_url_handler_queries.take(0);
        RefPtr<WebView::ExternalURLHandler> handler;
        if (has_handler)
            handler = adopt_ref(*new TestExternalURLHandler);
        query.callback(move(handler));
    }

private:
    struct PendingExternalURLHandlerQuery {
        URL::URL url;
        WebView::ExternalURLHandlerCallback callback;
    };

    mutable Vector<PendingExternalURLHandlerQuery> m_pending_external_url_handler_queries;
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
        .favicon_png = {},
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
                    .favicon_png = {},
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
        .favicon_png = {},
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

    auto expect_internal_url_suggestion = [&](WebView::AutocompleteQueryID query_id, StringView input) {
        auto query_completed = false;
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL);
            VERIFY(suggestions.first().text == input);
            autocomplete.cancel_pending_query();
            query_completed = true;
        };

        auto pending_handler_queries = app->pending_external_url_handler_query_count();
        autocomplete.query_autocomplete_engine(query_id, MUST(String::from_utf8(input)));
        VERIFY(app->pending_external_url_handler_query_count() == pending_handler_queries);
        Core::EventLoop::current().spin_until([&]() { return query_completed; });
    };

    expect_internal_url_suggestion(106, "localhost:8080"sv);
    expect_internal_url_suggestion(107, "example.com:8080"sv);

    auto telephone_url_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL);
            VERIFY(suggestions.first().text == "tel:123"sv);
            autocomplete.cancel_pending_query();
            telephone_url_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(108, "tel:123"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        VERIFY(app->pending_external_url_handler_query_scheme(0) == "tel"sv);
        app->complete_next_external_url_handler_query(true);
        Core::EventLoop::current().spin_until([&]() { return telephone_url_query_completed; });
    }

    auto external_url_local_query_completed = false;
    auto external_url_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.record_engagement({
            .input = "geo:23.4,12.4"_string,
            .destination_kind = WebView::OmniboxDestinationKind::URL,
            .destination = "https://ladybird.org/"_string,
            .was_explicit = true,
        });
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind == WebView::AutocompleteResultKind::Intermediate
                && suggestions.contains([](auto const& suggestion) {
                       return suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive
                           && suggestion.text == "https://ladybird.org/"sv;
                   })) {
                VERIFY(!suggestions.contains([](auto const& suggestion) {
                    return suggestion.source == WebView::AutocompleteSuggestionSource::LiteralURL;
                }));
                external_url_local_query_completed = true;
                return;
            }
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL);
            VERIFY(suggestions.first().text == "geo:23.4,12.4"sv);
            VERIFY(suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::Search
                    && suggestion.text == "geo:23.4,12.4"sv;
            }));
            autocomplete.cancel_pending_query();
            external_url_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(101, "geo:23.4,12.4"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        VERIFY(app->pending_external_url_handler_query_scheme(0) == "geo"sv);
        Core::EventLoop::current().spin_until([&]() { return external_url_local_query_completed; });
        app->complete_next_external_url_handler_query(true);
        Core::EventLoop::current().spin_until([&]() { return external_url_query_completed; });
    }

    auto unavailable_external_url_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto, auto const& suggestions, WebView::AutocompleteResultKind kind) {
            if (kind != WebView::AutocompleteResultKind::Final)
                return;
            VERIFY(!suggestions.is_empty());
            VERIFY(suggestions.first().source == WebView::AutocompleteSuggestionSource::Search);
            VERIFY(suggestions.first().text == "unknown:value"sv);
            VERIFY(!suggestions.contains([](auto const& suggestion) {
                return suggestion.source == WebView::AutocompleteSuggestionSource::LiteralURL;
            }));
            autocomplete.cancel_pending_query();
            unavailable_external_url_query_completed = true;
        };
        autocomplete.query_autocomplete_engine(102, "unknown:value"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        VERIFY(app->pending_external_url_handler_query_scheme(0) == "unknown"sv);
        app->complete_next_external_url_handler_query(false);
        Core::EventLoop::current().spin_until([&]() { return unavailable_external_url_query_completed; });
    }

    auto replacement_query_completed = false;
    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.on_autocomplete_query_complete = [&](auto query_id, auto const&, WebView::AutocompleteResultKind kind) {
            VERIFY(query_id != 103);
            if (query_id == 104 && kind == WebView::AutocompleteResultKind::Final) {
                autocomplete.cancel_pending_query();
                replacement_query_completed = true;
            }
        };
        autocomplete.query_autocomplete_engine(103, "geo:23.4,12.4"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
        autocomplete.query_autocomplete_engine(104, "replacement query"_string);
        app->complete_next_external_url_handler_query(true);
        Core::EventLoop::current().spin_until([&]() { return replacement_query_completed; });
    }

    {
        WebView::Autocomplete autocomplete { WebView::IsPrivate::No };
        autocomplete.query_autocomplete_engine(105, "geo:23.4,12.4"_string);
        VERIFY(app->pending_external_url_handler_query_count() == 1);
    }
    app->complete_next_external_url_handler_query(true);

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
